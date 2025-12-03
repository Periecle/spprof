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
#include "code_registry.h"

/* Memory profiler */
#include "memprof/memprof.h"
#include "memprof/heap_map.h"
#include "memprof/stack_intern.h"
#include "memprof/stack_capture.h"

/*
 * Include internal headers for free-threading detection.
 * The SPPROF_FREE_THREADING_SAFE macro is defined in pycore_frame.h.
 *
 * We always use the internal API for frame walking on all supported
 * Python versions (3.9-3.14).
 */
#include "internal/pycore_frame.h"
#include "internal/pycore_tstate.h"

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
    /* Note: PyArg_ParseTupleAndKeywords expects char**, not const char**.
     * This is a limitation of Python's C API. We suppress the warning locally.
     * GCC uses -Wdiscarded-qualifiers, Clang uses -Wincompatible-pointer-types-discards-qualifiers. */
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wwrite-strings"
#pragma clang diagnostic ignored "-Wincompatible-pointer-types-discards-qualifiers"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wwrite-strings"
#pragma GCC diagnostic ignored "-Wdiscarded-qualifiers"
#endif
    static char* kwlist[] = {"interval_ns", NULL};
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
    uint64_t interval_ns = 10000000;  /* Default 10ms */

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|K", kwlist, &interval_ns)) {
        return NULL;
    }

    /*
     * FREE-THREADING SAFETY CHECK
     *
     * On free-threaded Python builds (Py_GIL_DISABLED), signal-based sampling
     * is unsafe because the GIL is not there to protect frame chain consistency.
     *
     * Darwin/macOS uses Mach-based sampling (thread suspension) which IS safe.
     * Linux/other platforms use SIGPROF which is NOT safe for free-threading.
     *
     * We check SPPROF_FREE_THREADING_SAFE at compile time and fail early
     * with a clear error message if the configuration is unsafe.
     */
#if !SPPROF_FREE_THREADING_SAFE
    PyErr_SetString(PyExc_RuntimeError,
        "spprof is not supported on free-threaded Python builds on this platform. "
        "Free-threaded Python (--disable-gil) removes the GIL, making signal-based "
        "sampling unsafe because frame chains can be modified concurrently. "
        "On macOS, the Mach sampler is used instead and IS safe. "
        "On Linux, consider using alternative profilers like py-spy or scalene."
    );
    return NULL;
#endif

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
 * _stop_timer() - Stop the profiling timer without draining samples
 *
 * Internal function for streaming API. Stops sampling but leaves samples
 * in the buffer for chunked retrieval via _drain_buffer().
 *
 * Use this with _drain_buffer() for memory-efficient streaming of large profiles.
 */
static PyObject* spprof_stop_timer(PyObject* self, PyObject* args) {
    if (!ATOMIC_LOAD(&g_is_active)) {
        PyErr_SetString(PyExc_RuntimeError, "Profiler not running");
        return NULL;
    }

    /* Stop the timer */
    platform_timer_destroy();
    ATOMIC_STORE(&g_is_active, 0);

    Py_RETURN_NONE;
}

/**
 * _finalize_stop() - Clean up after draining all samples
 *
 * Call this after using _stop_timer() and _drain_buffer() to clean up
 * the resolver state.
 */
static PyObject* spprof_finalize_stop(PyObject* self, PyObject* args) {
    resolver_shutdown();
    Py_RETURN_NONE;
}

/**
 * _stop() - Stop profiling and return raw samples (legacy API)
 *
 * WARNING: For long profiling sessions with millions of samples, this can
 * cause OOM. Use _stop_timer() + _drain_buffer() + _finalize_stop() instead.
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
 *   - 'safe_mode_rejects': int (samples discarded due to safe mode)
 *   - 'validation_drops': int (samples dropped due to free-threading validation)
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
    
    /* Get safe mode rejection count from code registry */
    uint64_t safe_mode_rejects = 0;
    code_registry_get_stats_extended(NULL, NULL, NULL, NULL, NULL, &safe_mode_rejects);
    
    /* Get validation drop count (free-threading speculative capture) */
    uint64_t validation_drops = signal_handler_validation_drops();
    
    return Py_BuildValue(
        "{s:K, s:K, s:K, s:K, s:K, s:K}",
        "collected_samples", collected,
        "dropped_samples", dropped,
        "duration_ns", duration_ns,
        "interval_ns", g_interval_ns,
        "safe_mode_rejects", safe_mode_rejects,
        "validation_drops", validation_drops
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
 * _drain_buffer(max_samples) - Drain samples from buffer in chunks (streaming API)
 *
 * This allows retrieving samples in chunks without loading all into memory,
 * preventing OOM for long profiling sessions.
 *
 * Args:
 *   max_samples: Maximum number of samples to retrieve (default 10000)
 *
 * Returns:
 *   Tuple of (samples_list, has_more) where samples_list is a list of sample dicts
 *   and has_more is True if more samples are available.
 */
static PyObject* spprof_drain_buffer(PyObject* self, PyObject* args) {
    size_t max_samples = 10000;  /* Default batch size */

    if (!PyArg_ParseTuple(args, "|n", &max_samples)) {
        return NULL;
    }

    ResolvedSample* samples = NULL;
    size_t count = 0;

    if (resolver_drain_samples(max_samples, &samples, &count) < 0) {
        PyErr_SetString(PyExc_RuntimeError, "Failed to drain samples from buffer");
        return NULL;
    }

    /* Convert to Python list */
    PyObject* result_list = PyList_New((Py_ssize_t)count);
    if (result_list == NULL) {
        if (samples) free(samples);
        return NULL;
    }

    for (size_t i = 0; i < count; i++) {
        ResolvedSample* sample = &samples[i];

        /* Create frames list */
        PyObject* frames_list = PyList_New(sample->depth);
        if (frames_list == NULL) {
            Py_DECREF(result_list);
            if (samples) free(samples);
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
                Py_DECREF(result_list);
                if (samples) free(samples);
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
            Py_DECREF(result_list);
            if (samples) free(samples);
            return NULL;
        }

        PyList_SET_ITEM(result_list, (Py_ssize_t)i, sample_dict);
    }

    /* Free the samples array (caller owns it from resolver_drain_samples) */
    if (samples) {
        free(samples);
    }

    /* Check if more samples are available */
    int has_more = resolver_has_pending_samples();

    /* Return tuple of (samples_list, has_more) */
    return Py_BuildValue("(Oi)", result_list, has_more);
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

/**
 * _set_safe_mode(enabled) - Enable or disable safe mode for code validation
 *
 * When safe mode is enabled, code objects that were captured without holding
 * a reference (e.g., signal-handler captured samples on Linux) will be
 * discarded rather than validated via PyCode_Check.
 *
 * This trades sample completeness for guaranteed memory safety in production
 * environments where the tiny theoretical risk of accessing freed memory
 * is unacceptable.
 *
 * Note: Darwin/Mach samples are always safe (INCREF'd during capture).
 * Safe mode only affects Linux signal-handler samples.
 */
static PyObject* spprof_set_safe_mode(PyObject* self, PyObject* args) {
    int enabled;

    if (!PyArg_ParseTuple(args, "p", &enabled)) {
        return NULL;
    }

    code_registry_set_safe_mode(enabled);
    Py_RETURN_NONE;
}

/**
 * _is_safe_mode() - Check if safe mode is enabled
 */
static PyObject* spprof_is_safe_mode(PyObject* self, PyObject* args) {
    if (code_registry_is_safe_mode()) {
        Py_RETURN_TRUE;
    }
    Py_RETURN_FALSE;
}

/**
 * _get_code_registry_stats() - Get code registry statistics
 *
 * Returns a dict with detailed code registry statistics including
 * safe mode rejection count.
 */
static PyObject* spprof_get_code_registry_stats(PyObject* self, PyObject* args) {
    uint64_t refs_held = 0;
    uint64_t refs_added = 0;
    uint64_t refs_released = 0;
    uint64_t validations = 0;
    uint64_t invalid_count = 0;
    uint64_t safe_mode_rejects = 0;

    code_registry_get_stats_extended(
        &refs_held,
        &refs_added,
        &refs_released,
        &validations,
        &invalid_count,
        &safe_mode_rejects
    );

    return Py_BuildValue(
        "{s:K, s:K, s:K, s:K, s:K, s:K, s:O}",
        "refs_held", refs_held,
        "refs_added", refs_added,
        "refs_released", refs_released,
        "validations", validations,
        "invalid_count", invalid_count,
        "safe_mode_rejects", safe_mode_rejects,
        "safe_mode_enabled", code_registry_is_safe_mode() ? Py_True : Py_False
    );
}

/* ============================================================================
 * Memory Profiler Python Bindings
 * ============================================================================ */


/**
 * _memprof_init(sampling_rate_bytes) - Initialize memory profiler
 */
static PyObject* spprof_memprof_init(PyObject* self, PyObject* args) {
    uint64_t sampling_rate = MEMPROF_DEFAULT_SAMPLING_RATE;
    
    if (!PyArg_ParseTuple(args, "|K", &sampling_rate)) {
        return NULL;
    }
    
    int result = memprof_init(sampling_rate);
    return PyLong_FromLong(result);
}

/**
 * _memprof_start() - Start memory profiling
 */
static PyObject* spprof_memprof_start(PyObject* self, PyObject* args) {
    int result = memprof_start();
    return PyLong_FromLong(result);
}

/**
 * _memprof_stop() - Stop memory profiling (new allocations only)
 */
static PyObject* spprof_memprof_stop(PyObject* self, PyObject* args) {
    int result = memprof_stop();
    return PyLong_FromLong(result);
}

/**
 * _memprof_shutdown() - Shutdown memory profiler
 */
static PyObject* spprof_memprof_shutdown(PyObject* self, PyObject* args) {
    memprof_shutdown();
    Py_RETURN_NONE;
}

/**
 * _memprof_get_stats() - Get memory profiler statistics
 */
static PyObject* spprof_memprof_get_stats(PyObject* self, PyObject* args) {
    MemProfStats stats;
    
    if (memprof_get_stats(&stats) != 0) {
        Py_RETURN_NONE;
    }
    
    return Py_BuildValue(
        "{s:K, s:K, s:K, s:I, s:K, s:f, s:K, s:K, s:K, s:K, s:K}",
        "total_samples", (unsigned long long)stats.total_samples,
        "live_samples", (unsigned long long)stats.live_samples,
        "freed_samples", (unsigned long long)stats.freed_samples,
        "unique_stacks", (unsigned int)stats.unique_stacks,
        "estimated_heap_bytes", (unsigned long long)stats.estimated_heap_bytes,
        "heap_map_load_percent", stats.heap_map_load_percent,
        "collisions", (unsigned long long)stats.collisions,
        "sampling_rate_bytes", (unsigned long long)stats.sampling_rate_bytes,
        "shallow_stack_warnings", (unsigned long long)stats.shallow_stack_warnings,
        "death_during_birth", (unsigned long long)stats.death_during_birth,
        "zombie_races_detected", (unsigned long long)stats.zombie_races_detected
    );
}

/**
 * _memprof_get_snapshot() - Get snapshot of live allocations
 */
static PyObject* spprof_memprof_get_snapshot(PyObject* self, PyObject* args) {
    HeapMapEntry* entries = NULL;
    size_t count = 0;
    
    if (memprof_get_snapshot(&entries, &count) != 0) {
        PyErr_SetString(PyExc_RuntimeError, "Failed to get memory snapshot");
        return NULL;
    }
    
    /* Build result dict */
    PyObject* result = PyDict_New();
    if (!result) {
        memprof_free_snapshot(entries);
        return NULL;
    }
    
    /* Build entries list */
    PyObject* entries_list = PyList_New((Py_ssize_t)count);
    if (!entries_list) {
        Py_DECREF(result);
        memprof_free_snapshot(entries);
        return NULL;
    }
    
    for (size_t i = 0; i < count; i++) {
        HeapMapEntry* entry = &entries[i];
        
        uintptr_t ptr = atomic_load(&entry->ptr);
        uint64_t metadata = atomic_load(&entry->metadata);
        uint64_t birth_seq = atomic_load(&entry->birth_seq);
        uint64_t timestamp = entry->timestamp;
        
        uint32_t stack_id = METADATA_STACK_ID(metadata);
        uint32_t size = METADATA_SIZE(metadata);
        uint32_t weight = METADATA_WEIGHT(metadata);
        
        /* Build stack frames list */
        PyObject* stack_list = PyList_New(0);
        if (!stack_list) {
            Py_DECREF(entries_list);
            Py_DECREF(result);
            memprof_free_snapshot(entries);
            return NULL;
        }
        
        /* Get resolved stack if available */
        const StackEntry* stack_entry = stack_table_get(stack_id);
        if (stack_entry && (stack_entry->flags & STACK_FLAG_RESOLVED)) {
            for (int j = 0; j < stack_entry->depth; j++) {
                PyObject* frame_dict = Py_BuildValue(
                    "{s:K, s:s, s:s, s:i, s:O}",
                    "address", (unsigned long long)stack_entry->frames[j],
                    "function", stack_entry->function_names ? 
                                stack_entry->function_names[j] : "<unknown>",
                    "file", stack_entry->file_names ?
                            stack_entry->file_names[j] : "<unknown>",
                    "line", stack_entry->line_numbers ?
                            stack_entry->line_numbers[j] : 0,
                    "is_python", Py_False
                );
                
                if (!frame_dict || PyList_Append(stack_list, frame_dict) < 0) {
                    Py_XDECREF(frame_dict);
                    Py_DECREF(stack_list);
                    Py_DECREF(entries_list);
                    Py_DECREF(result);
                    memprof_free_snapshot(entries);
                    return NULL;
                }
                Py_DECREF(frame_dict);
            }
        }
        
        /* Build entry dict */
        PyObject* entry_dict = Py_BuildValue(
            "{s:K, s:I, s:I, s:K, s:K, s:O}",
            "address", (unsigned long long)ptr,
            "size", (unsigned int)size,
            "weight", (unsigned int)weight,
            "timestamp_ns", (unsigned long long)timestamp,
            "birth_seq", (unsigned long long)birth_seq,
            "stack", stack_list
        );
        
        Py_DECREF(stack_list);
        
        if (!entry_dict) {
            Py_DECREF(entries_list);
            Py_DECREF(result);
            memprof_free_snapshot(entries);
            return NULL;
        }
        
        PyList_SET_ITEM(entries_list, (Py_ssize_t)i, entry_dict);
    }
    
    /* Add entries to result */
    PyDict_SetItemString(result, "entries", entries_list);
    Py_DECREF(entries_list);
    
    /* Get frame pointer health */
    uint64_t shallow_warnings = 0, total_stacks = 0;
    float avg_depth = 0.0f;
    int min_depth = 0;
    get_frame_pointer_health(&shallow_warnings, &total_stacks, &avg_depth, &min_depth);
    
    PyObject* fp_health = Py_BuildValue(
        "{s:K, s:K, s:f, s:i}",
        "shallow_stack_warnings", (unsigned long long)shallow_warnings,
        "total_native_stacks", (unsigned long long)total_stacks,
        "avg_native_depth", avg_depth,
        "min_native_depth", min_depth
    );
    
    if (fp_health) {
        PyDict_SetItemString(result, "frame_pointer_health", fp_health);
        Py_DECREF(fp_health);
    }
    
    /* Add total samples */
    MemProfStats stats;
    if (memprof_get_stats(&stats) == 0) {
        PyDict_SetItemString(result, "total_samples", 
                             PyLong_FromUnsignedLongLong(stats.total_samples));
    }
    
    memprof_free_snapshot(entries);
    return result;
}

/* Method table */
static PyMethodDef SpProfMethods[] = {
    {"_start", (PyCFunction)(void(*)(void))spprof_start, METH_VARARGS | METH_KEYWORDS,
     "Start profiling (internal). Use spprof.start() instead."},
    {"_stop", spprof_stop, METH_NOARGS,
     "Stop profiling and return raw samples (internal, legacy API)."},
    {"_stop_timer", spprof_stop_timer, METH_NOARGS,
     "Stop the profiling timer without draining samples (streaming API)."},
    {"_finalize_stop", spprof_finalize_stop, METH_NOARGS,
     "Clean up resolver after streaming drain is complete."},
    {"_drain_buffer", spprof_drain_buffer, METH_VARARGS,
     "Drain samples from buffer in chunks (streaming API). Returns (samples, has_more)."},
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
    {"_set_safe_mode", spprof_set_safe_mode, METH_VARARGS,
     "Enable or disable safe mode for code validation."},
    {"_is_safe_mode", spprof_is_safe_mode, METH_NOARGS,
     "Check if safe mode is enabled."},
    {"_get_code_registry_stats", spprof_get_code_registry_stats, METH_NOARGS,
     "Get code registry statistics including safe mode rejects."},
    /* Memory profiler methods */
    {"_memprof_init", spprof_memprof_init, METH_VARARGS,
     "Initialize memory profiler with sampling rate."},
    {"_memprof_start", spprof_memprof_start, METH_NOARGS,
     "Start memory profiling."},
    {"_memprof_stop", spprof_memprof_stop, METH_NOARGS,
     "Stop memory profiling (new allocations only)."},
    {"_memprof_shutdown", spprof_memprof_shutdown, METH_NOARGS,
     "Shutdown memory profiler."},
    {"_memprof_get_stats", spprof_memprof_get_stats, METH_NOARGS,
     "Get memory profiler statistics."},
    {"_memprof_get_snapshot", spprof_memprof_get_snapshot, METH_NOARGS,
     "Get snapshot of live allocations."},
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

    /*
     * Initialize speculative capture for free-threaded Linux builds.
     *
     * This caches the PyCode_Type pointer for async-signal-safe type checking.
     * Must be called during module initialization (with GIL held) before any
     * signal handlers run.
     */
#if SPPROF_FREE_THREADED && defined(__linux__)
    if (_spprof_speculative_init() < 0) {
        platform_cleanup();
        Py_DECREF(module);
        PyErr_SetString(PyExc_RuntimeError,
            "Failed to initialize speculative capture for free-threaded Python");
        return NULL;
    }
#endif

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

    /*
     * Add free-threading support information.
     * This allows Python code to check if the profiler is safe to use.
     */
#ifdef Py_GIL_DISABLED
    if (PyModule_AddIntConstant(module, "free_threaded_build", 1) < 0) {
        platform_cleanup();
        Py_DECREF(module);
        return NULL;
    }
#else
    if (PyModule_AddIntConstant(module, "free_threaded_build", 0) < 0) {
        platform_cleanup();
        Py_DECREF(module);
        return NULL;
    }
#endif

#if SPPROF_FREE_THREADING_SAFE
    if (PyModule_AddIntConstant(module, "free_threading_safe", 1) < 0) {
        platform_cleanup();
        Py_DECREF(module);
        return NULL;
    }
#else
    if (PyModule_AddIntConstant(module, "free_threading_safe", 0) < 0) {
        platform_cleanup();
        Py_DECREF(module);
        return NULL;
    }
#endif

    return module;
}

