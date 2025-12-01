/**
 * framewalker.c - Unified version-polymorphic Python frame walking
 *
 * This implementation supports two modes via compile-time dispatch:
 *
 * 1. INTERNAL API MODE (SPPROF_USE_INTERNAL_API defined):
 *    - Direct internal structure access for async-signal-safe frame walking
 *    - Required for production signal-based sampling
 *    - Python 3.11+ only
 *
 * 2. PUBLIC API MODE (SPPROF_USE_INTERNAL_API not defined):
 *    - Uses public Python C API with version-specific compat headers
 *    - NOT async-signal-safe (Py_DECREF, etc. are called)
 *    - Works for testing and Python 3.9/3.10 fallback
 *
 * CRITICAL SAFETY PROPERTIES (internal API mode):
 *   - All capture functions are async-signal-safe
 *   - No Python API calls during sampling
 *   - No memory allocation
 *   - No locks
 *   - Direct struct field access only
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
 * Mode Selection and Header Includes
 * =============================================================================
 */

#ifdef SPPROF_USE_INTERNAL_API
    /* Production mode: async-signal-safe internal API */
    #include "internal/pycore_frame.h"
    #include "internal/pycore_tstate.h"
    #define FRAMEWALKER_MODE "internal"
#else
    /* Fallback mode: public API with version dispatch (NOT async-signal-safe) */
    #if PY_VERSION_HEX >= 0x030E0000  /* Python 3.14+ */
        #include "compat/py314.h"
        #define WALKER_VERSION "py314"
    #elif PY_VERSION_HEX >= 0x030D0000  /* Python 3.13 */
        #include "compat/py313.h"
        #define WALKER_VERSION "py313"
    #elif PY_VERSION_HEX >= 0x030C0000  /* Python 3.12 */
        #include "compat/py312.h"
        #define WALKER_VERSION "py312"
    #elif PY_VERSION_HEX >= 0x030B0000  /* Python 3.11 */
        #include "compat/py311.h"
        #define WALKER_VERSION "py311"
    #else  /* Python 3.9, 3.10 */
        #include "compat/py39.h"
        #define WALKER_VERSION "py39"
    #endif
    #define FRAMEWALKER_MODE "public-api"
#endif

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
 */

#ifdef SPPROF_USE_INTERNAL_API

/**
 * Get current frame from thread state - ASYNC-SIGNAL-SAFE
 */
static void* internal_get_current_frame(PyThreadState* tstate) {
    if (tstate == NULL) return NULL;
    return (void*)_spprof_get_current_frame(tstate);
}

/**
 * Get previous frame in chain - ASYNC-SIGNAL-SAFE
 */
static void* internal_get_previous_frame(void* frame) {
    if (frame == NULL) return NULL;
    return (void*)_spprof_frame_get_previous((_spprof_InterpreterFrame*)frame);
}

/**
 * Get code object address - ASYNC-SIGNAL-SAFE
 */
static uintptr_t internal_get_code_addr(void* frame) {
    if (frame == NULL) return 0;
    PyCodeObject* code = _spprof_frame_get_code((_spprof_InterpreterFrame*)frame);
    if (code == NULL) return 0;
    return (uintptr_t)code;
}

/**
 * Check if frame is a shim - ASYNC-SIGNAL-SAFE
 */
static int internal_is_shim_frame(void* frame) {
    if (frame == NULL) return 0;
    return _spprof_frame_is_shim((_spprof_InterpreterFrame*)frame);
}

#else /* !SPPROF_USE_INTERNAL_API */

/*
 * =============================================================================
 * Public API Implementation (NOT async-signal-safe)
 * =============================================================================
 *
 * These use the version-specific compat headers which call Python C API functions.
 * The compat_* functions are defined as static inline in the included header.
 */

static void* public_get_current_frame(PyThreadState* tstate) {
    return compat_get_current_frame(tstate);
}

static void* public_get_previous_frame(void* frame) {
    return compat_get_previous_frame(frame);
}

static uintptr_t public_get_code_addr(void* frame) {
    return compat_get_code_addr(frame);
}

static int public_is_shim_frame(void* frame) {
    return compat_is_shim_frame(frame);
}

#endif /* SPPROF_USE_INTERNAL_API */

/*
 * =============================================================================
 * Initialization
 * =============================================================================
 */

int framewalker_init(void) {
    if (g_initialized) {
        return 0;
    }

#ifdef SPPROF_USE_INTERNAL_API
    /* Set up internal API function pointers */
    g_vtable.get_current_frame = internal_get_current_frame;
    g_vtable.get_previous_frame = internal_get_previous_frame;
    g_vtable.get_code_addr = internal_get_code_addr;
    g_vtable.is_shim_frame = internal_is_shim_frame;
    
    snprintf(g_version_info, sizeof(g_version_info),
             "internal-api (Python %d.%d.%d, %s mode)",
             PY_MAJOR_VERSION, PY_MINOR_VERSION, PY_MICRO_VERSION,
             FRAMEWALKER_MODE);
#else
    /* Set up public API function pointers */
    g_vtable.get_current_frame = public_get_current_frame;
    g_vtable.get_previous_frame = public_get_previous_frame;
    g_vtable.get_code_addr = public_get_code_addr;
    g_vtable.is_shim_frame = public_is_shim_frame;
    
    snprintf(g_version_info, sizeof(g_version_info),
             "%s (Python %d.%d.%d) WARNING: not async-signal-safe",
             WALKER_VERSION,
             PY_MAJOR_VERSION, PY_MINOR_VERSION, PY_MICRO_VERSION);
#endif

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
 * In internal API mode, this is fully async-signal-safe.
 *
 * @param frame_ptrs Output array of code object pointers.
 * @param max_depth Maximum number of frames.
 * @return Number of frames captured.
 */
int framewalker_capture_raw(uintptr_t* frame_ptrs, int max_depth) {
    if (!g_initialized || frame_ptrs == NULL || max_depth <= 0) {
        return 0;
    }

#ifdef SPPROF_USE_INTERNAL_API
    /* Use the fully async-signal-safe implementation */
    return _spprof_capture_frames_unsafe(frame_ptrs, max_depth);
#else
    /* Fallback to public API (unsafe in signal handlers but functional for testing) */
    PyThreadState* tstate = PyThreadState_GET();
    if (tstate == NULL) {
        return 0;
    }

    void* frame = g_vtable.get_current_frame(tstate);
    int depth = 0;

    while (frame != NULL && depth < max_depth) {
        if (!g_vtable.is_shim_frame(frame)) {
            frame_ptrs[depth] = g_vtable.get_code_addr(frame);
            depth++;
        }
        frame = g_vtable.get_previous_frame(frame);
    }

    return depth;
#endif
}

/**
 * Capture frames with full frame info
 *
 * @param frames Output array to fill with frame info.
 * @param max_depth Maximum number of frames to capture.
 * @return Number of frames captured (0 to max_depth).
 */
int framewalker_capture(RawFrameInfo* frames, int max_depth) {
    if (!g_initialized || frames == NULL || max_depth <= 0) {
        return 0;
    }

#ifdef SPPROF_USE_INTERNAL_API
    /* Get thread state and walk frames using internal API */
    PyThreadState* tstate = _spprof_tstate_get();
    if (tstate == NULL) {
        return 0;
    }

    _spprof_InterpreterFrame* frame = _spprof_get_current_frame(tstate);
    int depth = 0;

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

    return depth;
#else
    /* Fallback to public API */
    PyThreadState* tstate = PyThreadState_GET();
    if (tstate == NULL) {
        return 0;
    }

    void* frame = g_vtable.get_current_frame(tstate);
    int depth = 0;

    while (frame != NULL && depth < max_depth) {
        frames[depth].is_shim = g_vtable.is_shim_frame(frame);
        if (!frames[depth].is_shim) {
            frames[depth].code_addr = g_vtable.get_code_addr(frame);
            depth++;
        }
        frame = g_vtable.get_previous_frame(frame);
    }

    return depth;
#endif
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

#ifdef SPPROF_USE_INTERNAL_API

/**
 * Capture mixed Python and native frames - ASYNC-SIGNAL-SAFE
 *
 * This interleaves Python frames with native C frames to provide a complete
 * picture of the call stack.
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

#endif /* SPPROF_USE_INTERNAL_API */

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
    
#ifdef SPPROF_USE_INTERNAL_API
    fprintf(stderr, "  Using internal API: YES\n");
    #if SPPROF_PY311
    fprintf(stderr, "  Python version: 3.11.x\n");
    #elif SPPROF_PY312
    fprintf(stderr, "  Python version: 3.12.x\n");
    #elif SPPROF_PY313
    fprintf(stderr, "  Python version: 3.13.x\n");
    #elif SPPROF_PY314
    fprintf(stderr, "  Python version: 3.14.x\n");
    #endif
#else
    fprintf(stderr, "  Using internal API: NO (public API: %s)\n", WALKER_VERSION);
#endif
}

#endif /* SPPROF_DEBUG */
