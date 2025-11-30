/**
 * module.c - Python module definition for spprof._native
 *
 * This is the main entry point for the C extension. It defines:
 *   - Python method bindings (_start, _stop, _is_active, _get_stats)
 *   - Module initialization (PyInit__native)
 *   - Global state management
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>

/* Atomic operations - use platform-specific implementations */
#ifdef _WIN32
#include <windows.h>
/* On Windows, use volatile LONG with Interlocked functions */
#define ATOMIC_INT volatile LONG
#define ATOMIC_LOAD(ptr) InterlockedCompareExchange(ptr, 0, 0)
#define ATOMIC_STORE(ptr, val) InterlockedExchange(ptr, val)
#else
#include <stdatomic.h>
#define ATOMIC_INT atomic_int
#define ATOMIC_LOAD(ptr) atomic_load(ptr)
#define ATOMIC_STORE(ptr, val) atomic_store(ptr, val)
#endif

#include "ringbuffer.h"
#include "framewalker.h"
#include "resolver.h"
#include "unwind.h"
#include "platform/platform.h"
#include "signal_handler.h"

/* Global state - exposed for platform signal handlers */
/* Must be visible for signal_handler.c to access via extern */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((visibility("default")))
#endif
RingBuffer* g_ringbuffer = NULL;
static ATOMIC_INT g_is_active = 0;
static uint64_t g_start_time = 0;
static uint64_t g_interval_ns = 0;
static int g_module_initialized = 0;

/* Forward declaration for cleanup */
static void spprof_cleanup(void);

/**
 * _start(interval_ns) - Start profiling
 *
 * Internal function. Use spprof.start() from Python.
 */
static PyObject* spprof_start(PyObject* self, PyObject* args, PyObject* kwargs) {
    static char* kwlist[] = {"interval_ns", NULL};
    uint64_t interval_ns = 10000000;  /* Default 10ms */

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|K", kwlist, &interval_ns)) {
        return NULL;
    }

    /* Check if already running (atomic read) */
    if (ATOMIC_LOAD(&g_is_active)) {
        PyErr_SetString(PyExc_RuntimeError, "Profiler already running");
        return NULL;
    }

    /* Validate interval */
    if (interval_ns < 1000000) {  /* Minimum 1ms */
        PyErr_SetString(PyExc_ValueError, "interval_ns must be >= 1000000 (1ms)");
        return NULL;
    }

    /* Create ring buffer if needed */
    if (g_ringbuffer == NULL) {
        g_ringbuffer = ringbuffer_create();
        if (g_ringbuffer == NULL) {
            PyErr_SetString(PyExc_MemoryError, "Failed to allocate ring buffer");
            return NULL;
        }
    } else {
        ringbuffer_reset(g_ringbuffer);
    }

    /* Initialize resolver */
    if (resolver_init(g_ringbuffer) < 0) {
        PyErr_SetString(PyExc_OSError, "Failed to initialize resolver");
        return NULL;
    }

    /* Set up platform timer and signal handler */
    if (platform_timer_create(interval_ns) < 0) {
        resolver_shutdown();
        PyErr_SetString(PyExc_OSError, "Failed to create profiling timer");
        return NULL;
    }

    g_interval_ns = interval_ns;
    g_start_time = platform_monotonic_ns();
    ATOMIC_STORE(&g_is_active, 1);

    Py_RETURN_NONE;
}

/**
 * _stop() - Stop profiling and return raw samples
 *
 * Internal function. Use spprof.stop() from Python.
 *
 * Returns a list of dicts, each containing:
 *   - 'timestamp': int (nanoseconds)
 *   - 'thread_id': int
 *   - 'frames': list of dicts with 'function', 'filename', 'lineno', 'is_native'
 */
static PyObject* spprof_stop(PyObject* self, PyObject* args) {
    if (!ATOMIC_LOAD(&g_is_active)) {
        PyErr_SetString(PyExc_RuntimeError, "Profiler not running");
        return NULL;
    }

    /* Stop the timer */
    platform_timer_destroy();
    ATOMIC_STORE(&g_is_active, 0);

    /* Get resolved samples */
    ResolvedSample* samples = NULL;
    size_t count = 0;

    if (resolver_get_samples(&samples, &count) < 0) {
        resolver_shutdown();
        PyErr_SetString(PyExc_RuntimeError, "Failed to get resolved samples");
        return NULL;
    }

    /* Convert to Python list */
    PyObject* result = PyList_New((Py_ssize_t)count);
    if (result == NULL) {
        resolver_free_samples(samples, count);
        resolver_shutdown();
        return NULL;
    }

    for (size_t i = 0; i < count; i++) {
        ResolvedSample* sample = &samples[i];

        /* Create frames list */
        PyObject* frames_list = PyList_New(sample->depth);
        if (frames_list == NULL) {
            Py_DECREF(result);
            resolver_free_samples(samples, count);
            resolver_shutdown();
            return NULL;
        }

        for (int j = 0; j < sample->depth; j++) {
            ResolvedFrame* frame = &sample->frames[j];

            PyObject* frame_dict = Py_BuildValue(
                "{s:s, s:s, s:i, s:O}",
                "function", frame->function_name,
                "filename", frame->filename,
                "lineno", frame->lineno,
                "is_native", frame->is_native ? Py_True : Py_False
            );

            if (frame_dict == NULL) {
                Py_DECREF(frames_list);
                Py_DECREF(result);
                resolver_free_samples(samples, count);
                resolver_shutdown();
                return NULL;
            }

            PyList_SET_ITEM(frames_list, j, frame_dict);
        }

        /* Create sample dict */
        PyObject* sample_dict = Py_BuildValue(
            "{s:K, s:K, s:O}",
            "timestamp", sample->timestamp,
            "thread_id", sample->thread_id,
            "frames", frames_list
        );

        Py_DECREF(frames_list);

        if (sample_dict == NULL) {
            Py_DECREF(result);
            resolver_free_samples(samples, count);
            resolver_shutdown();
            return NULL;
        }

        PyList_SET_ITEM(result, (Py_ssize_t)i, sample_dict);
    }

    resolver_free_samples(samples, count);
    resolver_shutdown();

    return result;
}

/**
 * _is_active() - Check if profiler is running
 */
static PyObject* spprof_is_active(PyObject* self, PyObject* args) {
    if (ATOMIC_LOAD(&g_is_active)) {
        Py_RETURN_TRUE;
    }
    Py_RETURN_FALSE;
}

/**
 * _get_stats() - Get current profiling statistics
 *
 * Returns a dict with:
 *   - 'collected_samples': int
 *   - 'dropped_samples': int
 *   - 'duration_ns': int
 *   - 'interval_ns': int
 */
static PyObject* spprof_get_stats(PyObject* self, PyObject* args) {
    int is_active = ATOMIC_LOAD(&g_is_active);
    if (!is_active && g_ringbuffer == NULL) {
        Py_RETURN_NONE;
    }

    uint64_t current_time = platform_monotonic_ns();
    uint64_t duration_ns = is_active ? (current_time - g_start_time) : 0;
    uint64_t dropped = g_ringbuffer ? ringbuffer_dropped_count(g_ringbuffer) : 0;

    uint64_t collected = signal_handler_samples_captured();
    
    return Py_BuildValue(
        "{s:K, s:K, s:K, s:K}",
        "collected_samples", collected,
        "dropped_samples", dropped,
        "duration_ns", duration_ns,
        "interval_ns", g_interval_ns
    );
}

/**
 * _register_thread() - Register current thread for sampling
 *
 * On Linux with timer_create, each thread needs its own timer.
 * Returns True if registered or not needed, False otherwise.
 */
static PyObject* spprof_register_thread(PyObject* self, PyObject* args) {
    /* If profiler is not active, this is a no-op - return True */
    if (!ATOMIC_LOAD(&g_is_active)) {
        Py_RETURN_TRUE;
    }

    if (platform_register_thread(g_interval_ns) < 0) {
        /* On failure, return False rather than raising exception */
        Py_RETURN_FALSE;
    }

    Py_RETURN_TRUE;
}

/**
 * _unregister_thread() - Unregister current thread from sampling
 *
 * Call before thread exits to clean up resources.
 * Returns True if unregistered or not needed, False otherwise.
 */
static PyObject* spprof_unregister_thread(PyObject* self, PyObject* args) {
    /* If profiler is not active, this is a no-op - return True */
    if (!ATOMIC_LOAD(&g_is_active)) {
        Py_RETURN_TRUE;
    }

    if (platform_unregister_thread() < 0) {
        Py_RETURN_FALSE;
    }

    Py_RETURN_TRUE;
}

/**
 * _set_native_unwinding(enabled) - Enable/disable native stack unwinding
 *
 * When enabled, the profiler will also capture C/C++ frames using
 * libunwind (Linux) or backtrace (macOS).
 */
static PyObject* spprof_set_native_unwinding(PyObject* self, PyObject* args) {
    int enabled;

    if (!PyArg_ParseTuple(args, "p", &enabled)) {
        return NULL;
    }

    if (framewalker_set_native_unwinding(enabled) < 0) {
        PyErr_SetString(PyExc_RuntimeError,
            "Native unwinding not available on this platform");
        return NULL;
    }

    Py_RETURN_NONE;
}

/**
 * _native_unwinding_available() - Check if native unwinding is available
 */
static PyObject* spprof_native_unwinding_available(PyObject* self, PyObject* args) {
    if (framewalker_native_unwinding_available()) {
        Py_RETURN_TRUE;
    }
    Py_RETURN_FALSE;
}

/**
 * _native_unwinding_enabled() - Check if native unwinding is enabled
 */
static PyObject* spprof_native_unwinding_enabled(PyObject* self, PyObject* args) {
    if (framewalker_native_unwinding_enabled()) {
        Py_RETURN_TRUE;
    }
    Py_RETURN_FALSE;
}

/**
 * _capture_native_stack() - Capture current native stack (for testing)
 *
 * Returns a list of dicts with native frame info.
 */
static PyObject* spprof_capture_native_stack(PyObject* self, PyObject* args) {
    if (!framewalker_native_unwinding_available()) {
        PyErr_SetString(PyExc_RuntimeError,
            "Native unwinding not available on this platform");
        return NULL;
    }

    NativeStack stack;
    int captured = unwind_capture_with_symbols(&stack, 1);  /* Skip this function */

    if (captured < 0) {
        PyErr_SetString(PyExc_RuntimeError, "Failed to capture native stack");
        return NULL;
    }

    PyObject* result = PyList_New(stack.depth);
    if (result == NULL) {
        return NULL;
    }

    for (int i = 0; i < stack.depth; i++) {
        NativeFrame* frame = &stack.frames[i];

        PyObject* frame_dict = Py_BuildValue(
            "{s:K, s:s, s:s, s:K, s:O}",
            "ip", (unsigned long long)frame->ip,
            "symbol", frame->symbol,
            "filename", frame->filename,
            "offset", (unsigned long long)frame->offset,
            "resolved", frame->resolved ? Py_True : Py_False
        );

        if (frame_dict == NULL) {
            Py_DECREF(result);
            return NULL;
        }

        PyList_SET_ITEM(result, i, frame_dict);
    }

    return result;
}

/* Method table */
static PyMethodDef SpProfMethods[] = {
    {"_start", (PyCFunction)spprof_start, METH_VARARGS | METH_KEYWORDS,
     "Start profiling (internal). Use spprof.start() instead."},
    {"_stop", spprof_stop, METH_NOARGS,
     "Stop profiling and return raw samples (internal)."},
    {"_is_active", spprof_is_active, METH_NOARGS,
     "Check if profiling is active."},
    {"_get_stats", spprof_get_stats, METH_NOARGS,
     "Get current profiling statistics."},
    {"_register_thread", spprof_register_thread, METH_NOARGS,
     "Register current thread for per-thread sampling (Linux)."},
    {"_unregister_thread", spprof_unregister_thread, METH_NOARGS,
     "Unregister current thread from sampling."},
    {"_set_native_unwinding", spprof_set_native_unwinding, METH_VARARGS,
     "Enable or disable native C-stack unwinding."},
    {"_native_unwinding_available", spprof_native_unwinding_available, METH_NOARGS,
     "Check if native unwinding is available on this platform."},
    {"_native_unwinding_enabled", spprof_native_unwinding_enabled, METH_NOARGS,
     "Check if native unwinding is currently enabled."},
    {"_capture_native_stack", spprof_capture_native_stack, METH_NOARGS,
     "Capture current native stack (for testing)."},
    {NULL, NULL, 0, NULL}
};

/**
 * Module cleanup - called at interpreter shutdown
 */
static void spprof_cleanup(void) {
    /* Stop profiling if still running */
    if (ATOMIC_LOAD(&g_is_active)) {
        platform_timer_destroy();
        ATOMIC_STORE(&g_is_active, 0);
        resolver_shutdown();
    }
    
    /* Free ring buffer */
    if (g_ringbuffer != NULL) {
        ringbuffer_destroy(g_ringbuffer);
        g_ringbuffer = NULL;
    }
    
    /* Cleanup platform */
    platform_cleanup();
    
    g_module_initialized = 0;
}

/**
 * Module free function - called when module is deallocated
 */
static void spprof_module_free(void* module) {
    spprof_cleanup();
}

/* Module definition */
static struct PyModuleDef spprof_module = {
    PyModuleDef_HEAD_INIT,
    "_native",                               /* Module name */
    "spprof native C extension (internal)",  /* Module docstring */
    -1,                                      /* Size of per-interpreter state */
    SpProfMethods,
    NULL,                                    /* m_slots */
    NULL,                                    /* m_traverse */
    NULL,                                    /* m_clear */
    spprof_module_free                       /* m_free */
};

/**
 * Module initialization
 */
PyMODINIT_FUNC PyInit__native(void) {
    PyObject* module = PyModule_Create(&spprof_module);
    if (module == NULL) {
        return NULL;
    }

    /* Initialize platform subsystem */
    if (platform_init() < 0) {
        Py_DECREF(module);
        PyErr_SetString(PyExc_OSError, "Failed to initialize platform");
        return NULL;
    }

    /* Initialize frame walker */
    if (framewalker_init() < 0) {
        platform_cleanup();
        Py_DECREF(module);
        PyErr_SetString(PyExc_RuntimeError, "Unsupported Python version");
        return NULL;
    }

    /* Register atexit handler for cleanup */
    if (Py_AtExit(spprof_cleanup) < 0) {
        /* Non-fatal: cleanup will still happen via m_free */
    }

    g_module_initialized = 1;

    /* Add version info */
    if (PyModule_AddStringConstant(module, "__version__", "0.1.0") < 0) {
        platform_cleanup();
        Py_DECREF(module);
        return NULL;
    }

    if (PyModule_AddStringConstant(module, "platform", SPPROF_PLATFORM_NAME) < 0) {
        platform_cleanup();
        Py_DECREF(module);
        return NULL;
    }

    const char* frame_walker_info = framewalker_version_info();
    if (PyModule_AddStringConstant(module, "frame_walker", frame_walker_info) < 0) {
        platform_cleanup();
        Py_DECREF(module);
        return NULL;
    }

    /* Add native unwinding info */
    const char* unwind_method = unwind_method_name();
    if (PyModule_AddStringConstant(module, "unwind_method", unwind_method) < 0) {
        platform_cleanup();
        Py_DECREF(module);
        return NULL;
    }

    if (PyModule_AddIntConstant(module, "native_unwinding_available",
                                 unwind_available()) < 0) {
        platform_cleanup();
        Py_DECREF(module);
        return NULL;
    }

    return module;
}

