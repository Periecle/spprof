/**
 * framewalker.c - Async-signal-safe Python frame walking
 *
 * This implementation uses internal Python structure access exclusively
 * for all supported Python versions (3.9-3.14). The internal API provides:
 *
 *   - Async-signal-safe frame capture (required for signal-based sampling)
 *   - Consistent frame details across all Python versions
 *   - No Python C API calls during sampling (except PyCode_Check on 3.13+)
 *   - Single code path for simplified maintenance
 *
 * CRITICAL SAFETY PROPERTIES:
 *   - All capture functions are async-signal-safe
 *   - No memory allocation in capture paths
 *   - No locks in capture paths
 *   - Direct struct field access only
 *
 * Supported Python versions:
 *   - Python 3.9.x  (PyFrameObject via tstate->frame)
 *   - Python 3.10.x (PyFrameObject via tstate->frame)
 *   - Python 3.11.x (_PyInterpreterFrame via cframe)
 *   - Python 3.12.x (_PyInterpreterFrame via cframe)
 *   - Python 3.13.x (_PyInterpreterFrame via current_frame)
 *   - Python 3.14.x (_PyInterpreterFrame via current_frame)
 *
 * Copyright (c) 2024 spprof contributors
 * SPDX-License-Identifier: MIT
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <string.h>
#include <stdio.h>

#include "framewalker.h"
#include "unwind.h"

/*
 * =============================================================================
 * Internal API Headers
 * =============================================================================
 *
 * We always use the internal API for frame walking. This provides async-signal-
 * safe frame capture on all supported Python versions.
 */

#include "internal/pycore_frame.h"
#include "internal/pycore_tstate.h"

/* Mode identifier for version info string */
#define FRAMEWALKER_MODE "internal-api"

/*
 * =============================================================================
 * Global State
 * =============================================================================
 */

static int g_initialized = 0;
static char g_version_info[256];
static int g_native_unwinding_enabled = 0;

/* VTable for compatibility layer */
static FrameWalkerVTable g_vtable;

/*
 * =============================================================================
 * Internal API Implementation (Async-Signal-Safe)
 * =============================================================================
 *
 * All functions below are async-signal-safe and use direct struct access.
 * They work for all supported Python versions (3.9-3.14).
 */

/**
 * Get current frame from thread state - ASYNC-SIGNAL-SAFE
 */
static void* internal_get_current_frame(PyThreadState* tstate) {
    if (tstate == NULL) return NULL;
    return (void*)_spprof_get_current_frame(tstate);
}

/**
 * Get previous frame in chain - ASYNC-SIGNAL-SAFE
 *
 * Note: Uses version-specific frame type internally.
 */
static void* internal_get_previous_frame(void* frame) {
    if (frame == NULL) return NULL;
#if SPPROF_PY39 || SPPROF_PY310
    return (void*)_spprof_frame_get_previous((_spprof_PyFrameObject*)frame);
#else
    return (void*)_spprof_frame_get_previous((_spprof_InterpreterFrame*)frame);
#endif
}

/**
 * Get code object address - ASYNC-SIGNAL-SAFE
 *
 * Note: Uses version-specific frame type internally.
 */
static uintptr_t internal_get_code_addr(void* frame) {
    if (frame == NULL) return 0;
#if SPPROF_PY39 || SPPROF_PY310
    PyCodeObject* code = _spprof_frame_get_code((_spprof_PyFrameObject*)frame);
#else
    PyCodeObject* code = _spprof_frame_get_code((_spprof_InterpreterFrame*)frame);
#endif
    if (code == NULL) return 0;
    return (uintptr_t)code;
}

/**
 * Check if frame is a shim - ASYNC-SIGNAL-SAFE
 *
 * Note: Python 3.9/3.10 don't have shim frames, always returns 0.
 */
static int internal_is_shim_frame(void* frame) {
    if (frame == NULL) return 0;
#if SPPROF_PY39 || SPPROF_PY310
    /* Python 3.9/3.10 don't have shim frames */
    (void)frame;
    return 0;
#else
    return _spprof_frame_is_shim((_spprof_InterpreterFrame*)frame);
#endif
}

/*
 * =============================================================================
 * Initialization
 * =============================================================================
 */

int framewalker_init(void) {
    if (g_initialized) {
        return 0;
    }

    /* Set up internal API function pointers */
    g_vtable.get_current_frame = internal_get_current_frame;
    g_vtable.get_previous_frame = internal_get_previous_frame;
    g_vtable.get_code_addr = internal_get_code_addr;
    g_vtable.is_shim_frame = internal_is_shim_frame;
    
    snprintf(g_version_info, sizeof(g_version_info),
             "%s (Python %d.%d.%d)",
             FRAMEWALKER_MODE,
             PY_MAJOR_VERSION, PY_MINOR_VERSION, PY_MICRO_VERSION);

    g_initialized = 1;
    return 0;
}

/*
 * =============================================================================
 * Frame Capture Functions
 * =============================================================================
 */

/**
 * Capture frames as raw uintptr_t array - PRIMARY CAPTURE FUNCTION
 *
 * This is the primary capture function used by the signal handler.
 * It is fully async-signal-safe on all supported Python versions.
 *
 * ASYNC-SIGNAL-SAFE: Uses _spprof_capture_frames_unsafe() which performs
 * only direct memory reads without Python API calls.
 *
 * @param frame_ptrs Output array of code object pointers.
 * @param max_depth Maximum number of frames.
 * @return Number of frames captured.
 */
int framewalker_capture_raw(uintptr_t* frame_ptrs, int max_depth) {
    if (!g_initialized || frame_ptrs == NULL || max_depth <= 0) {
        return 0;
    }

    /* Use the fully async-signal-safe internal API implementation */
    return _spprof_capture_frames_unsafe(frame_ptrs, max_depth);
}

/**
 * Capture frames with full frame info
 *
 * ASYNC-SIGNAL-SAFE: Uses internal API with direct memory reads.
 *
 * @param frames Output array to fill with frame info.
 * @param max_depth Maximum number of frames to capture.
 * @return Number of frames captured (0 to max_depth).
 */
int framewalker_capture(RawFrameInfo* frames, int max_depth) {
    if (!g_initialized || frames == NULL || max_depth <= 0) {
        return 0;
    }

    /* Get thread state and walk frames using internal API */
    PyThreadState* tstate = _spprof_tstate_get();
    if (tstate == NULL) {
        return 0;
    }

    int depth = 0;

#if SPPROF_PY39 || SPPROF_PY310
    /* Python 3.9/3.10: Use PyFrameObject */
    _spprof_PyFrameObject* frame = _spprof_get_current_frame(tstate);

    while (frame != NULL && depth < max_depth) {
        if (!_spprof_ptr_valid(frame)) {
            break;
        }

        /* Python 3.9/3.10 don't have shim frames */
        frames[depth].is_shim = 0;
        
        PyCodeObject* code = _spprof_frame_get_code(frame);
        if (code != NULL && _spprof_ptr_valid(code)) {
            frames[depth].code_addr = (uintptr_t)code;
            depth++;
        }
        
        frame = _spprof_frame_get_previous(frame);
    }
#else
    /* Python 3.11+: Use _PyInterpreterFrame */
    _spprof_InterpreterFrame* frame = _spprof_get_current_frame(tstate);

    while (frame != NULL && depth < max_depth) {
        if (!_spprof_ptr_valid(frame)) {
            break;
        }

        int is_shim = _spprof_frame_is_shim(frame);
        frames[depth].is_shim = is_shim;
        
        if (!is_shim) {
            PyCodeObject* code = _spprof_frame_get_code(frame);
            if (code != NULL && _spprof_ptr_valid(code)) {
                frames[depth].code_addr = (uintptr_t)code;
                depth++;
            }
        }
        
        frame = _spprof_frame_get_previous(frame);
    }
#endif

    return depth;
}

/*
 * =============================================================================
 * Accessors
 * =============================================================================
 */

const char* framewalker_version_info(void) {
    return g_version_info;
}

const FrameWalkerVTable* framewalker_get_vtable(void) {
    return &g_vtable;
}

/*
 * =============================================================================
 * Native Unwinding Support
 * =============================================================================
 */

int framewalker_set_native_unwinding(int enabled) {
    if (enabled && !unwind_available()) {
        return -1;
    }

    if (enabled && !g_native_unwinding_enabled) {
        if (unwind_init() < 0) {
            return -1;
        }
    }

    g_native_unwinding_enabled = enabled;
    return 0;
}

int framewalker_native_unwinding_enabled(void) {
    return g_native_unwinding_enabled;
}

int framewalker_native_unwinding_available(void) {
    return unwind_available();
}

/*
 * =============================================================================
 * Mixed-Mode Capture (Python + Native frames)
 * =============================================================================
 */

/**
 * Capture mixed Python and native frames - ASYNC-SIGNAL-SAFE
 *
 * This interleaves Python frames with native C frames to provide a complete
 * picture of the call stack.
 *
 * ASYNC-SIGNAL-SAFE: Uses internal API for Python frames and libunwind/backtrace
 * for native frames.
 *
 * @param python_frames  Output array for Python frame code pointers
 * @param native_stack   Output structure for native frame data
 * @param max_py_depth   Maximum Python frames
 * @param py_count       Output: number of Python frames captured
 * @param native_count   Output: number of native frames captured
 */
void framewalker_capture_mixed(
    uintptr_t* python_frames,
    NativeStack* native_stack,
    int max_py_depth,
    int* py_count,
    int* native_count
) {
    /* Capture Python frames */
    *py_count = _spprof_capture_frames_unsafe(python_frames, max_py_depth);
    
    /* Capture native frames if enabled */
    if (g_native_unwinding_enabled && native_stack != NULL) {
        *native_count = unwind_capture(native_stack, 2);  /* Skip signal handler frames */
    } else {
        *native_count = 0;
    }
}

/*
 * =============================================================================
 * Debug Utilities
 * =============================================================================
 */

#ifdef SPPROF_DEBUG

/**
 * Print frame walker diagnostics (NOT async-signal-safe)
 */
void framewalker_debug_print(void) {
    fprintf(stderr, "[spprof] Frame Walker Diagnostics:\n");
    fprintf(stderr, "  Version: %s\n", g_version_info);
    fprintf(stderr, "  Mode: %s\n", FRAMEWALKER_MODE);
    fprintf(stderr, "  Initialized: %d\n", g_initialized);
    fprintf(stderr, "  Native unwinding: %s (enabled: %d)\n",
            unwind_available() ? "available" : "not available",
            g_native_unwinding_enabled);
    
    fprintf(stderr, "  Using internal API: YES\n");
#if SPPROF_PY39
    fprintf(stderr, "  Python version: 3.9.x\n");
#elif SPPROF_PY310
    fprintf(stderr, "  Python version: 3.10.x\n");
#elif SPPROF_PY311
    fprintf(stderr, "  Python version: 3.11.x\n");
#elif SPPROF_PY312
    fprintf(stderr, "  Python version: 3.12.x\n");
#elif SPPROF_PY313
    fprintf(stderr, "  Python version: 3.13.x\n");
#elif SPPROF_PY314
    fprintf(stderr, "  Python version: 3.14.x\n");
#else
    fprintf(stderr, "  Python version: unknown\n");
#endif
}

#endif /* SPPROF_DEBUG */

/* ============================================================================
 * Code Object Resolution (for memory profiler)
 * ============================================================================ */

/**
 * Resolve a code object pointer to function name, file name, and line number.
 *
 * REQUIRES GIL.
 *
 * @param code_ptr    Raw PyCodeObject* pointer
 * @param func_name   Output: allocated function name string (caller must free)
 * @param file_name   Output: allocated file name string (caller must free)
 * @param line_no     Output: first line number
 * @return 0 on success, -1 on error
 */
int resolve_code_object(uintptr_t code_ptr, char** func_name, char** file_name, int* line_no) {
    if (code_ptr == 0 || !func_name || !file_name || !line_no) {
        return -1;
    }
    
    *func_name = NULL;
    *file_name = NULL;
    *line_no = 0;
    
    /* Validate pointer alignment */
    if ((code_ptr & 0x7) != 0) {
        return -1;
    }
    
    PyCodeObject* code = (PyCodeObject*)code_ptr;
    
    /* Use PyCode_Check to validate - requires GIL */
    if (!PyCode_Check(code)) {
        return -1;
    }
    
    /* Get function name */
    PyObject* name_obj = code->co_qualname ? code->co_qualname : code->co_name;
    if (name_obj && PyUnicode_Check(name_obj)) {
        const char* name_str = PyUnicode_AsUTF8(name_obj);
        if (name_str) {
            *func_name = strdup(name_str);
        }
    }
    if (!*func_name) {
        *func_name = strdup("<unknown>");
    }
    
    /* Get file name */
    if (code->co_filename && PyUnicode_Check(code->co_filename)) {
        const char* file_str = PyUnicode_AsUTF8(code->co_filename);
        if (file_str) {
            *file_name = strdup(file_str);
        }
    }
    if (!*file_name) {
        *file_name = strdup("<unknown>");
    }
    
    /* Get first line number */
    *line_no = code->co_firstlineno;
    
    return 0;
}
