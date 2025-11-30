/**
 * internal/pycore_tstate.h - Thread state access for async-signal-safe sampling
 *
 * This header provides the core async-signal-safe functions for capturing
 * Python call stacks from within a signal handler context.
 *
 * ASYNC-SIGNAL-SAFETY REQUIREMENTS:
 *   - No malloc/free/realloc
 *   - No locks (pthread_mutex, etc.)
 *   - No stdio (printf, fprintf, fwrite)
 *   - No Python C API calls that acquire GIL
 *   - Only direct memory reads via pointers
 *   - TLS access (generally safe on modern systems)
 *
 * Copyright (c) 2024 spprof contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef SPPROF_INTERNAL_PYCORE_TSTATE_H
#define SPPROF_INTERNAL_PYCORE_TSTATE_H

#include <Python.h>
#include <stdint.h>
#include "pycore_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * =============================================================================
 * Thread State Access
 * =============================================================================
 *
 * Getting the current thread state from within a signal handler is the
 * critical first step. We use Python's TLS mechanism which is async-signal-safe
 * on modern platforms (Linux glibc, macOS, Windows).
 */

/**
 * Get current thread state - ASYNC-SIGNAL-SAFE
 *
 * This reads from thread-local storage without locks or allocation.
 * On Linux with glibc, __thread variables are async-signal-safe.
 */
static inline PyThreadState*
_spprof_tstate_get(void) {
#if PY_VERSION_HEX >= 0x030D0000
    /* Python 3.13+: _PyThreadState_GET() is a macro that reads TLS */
    return _PyThreadState_GET();
#elif PY_VERSION_HEX >= 0x030B0000
    /* Python 3.11-3.12: PyThreadState_GET() reads from _Py_tss_tstate */
    return PyThreadState_GET();
#else
    /* Older versions - shouldn't reach here */
    return PyThreadState_GET();
#endif
}

/*
 * =============================================================================
 * Pointer Validation
 * =============================================================================
 *
 * In a signal handler, pointers might be in inconsistent states. We use
 * simple heuristics to detect obviously invalid pointers.
 *
 * Note: This is NOT foolproof - it catches NULL and clearly bad addresses
 * but can't detect dangling pointers to valid-looking memory.
 */

/* Reasonable pointer bounds for 64-bit systems */
#if SIZEOF_VOID_P == 8
    #define SPPROF_PTR_MIN ((uintptr_t)0x1000)
    /* 47-bit user space on x86-64, 48-bit on ARM64 */
    #define SPPROF_PTR_MAX ((uintptr_t)0x00007FFFFFFFFFFF)
#else
    /* 32-bit systems */
    #define SPPROF_PTR_MIN ((uintptr_t)0x1000)
    #define SPPROF_PTR_MAX ((uintptr_t)0xFFFFFFFF)
#endif

/**
 * Quick pointer validation - ASYNC-SIGNAL-SAFE
 *
 * Returns 1 if pointer looks reasonable, 0 otherwise.
 * This catches NULL and obviously invalid addresses.
 */
static inline int
_spprof_ptr_valid(const void *ptr) {
    uintptr_t addr = (uintptr_t)ptr;
    return addr >= SPPROF_PTR_MIN && addr <= SPPROF_PTR_MAX;
}

/*
 * =============================================================================
 * Frame Stack Capture
 * =============================================================================
 */

/**
 * Capture Python frame code pointers - ASYNC-SIGNAL-SAFE
 *
 * This is THE core function called from the signal handler.
 * It walks the Python frame chain and extracts raw PyCodeObject pointers
 * for later symbol resolution.
 *
 * Safety guarantees:
 *   - No Python API calls
 *   - No memory allocation
 *   - No locks
 *   - Direct struct access only
 *   - Early exit on invalid pointers
 *
 * @param frames Output array for code object pointers
 * @param max_frames Maximum number of frames to capture
 * @return Number of frames captured (0 to max_frames)
 */

static inline int
_spprof_capture_frames_unsafe(uintptr_t *frames, int max_frames) {
    if (frames == NULL || max_frames <= 0) {
        return 0;
    }

    /* Get thread state from TLS */
    PyThreadState *tstate = _spprof_tstate_get();
    if (!_spprof_ptr_valid(tstate)) {
        return 0;
    }

    /* Get current interpreter frame */
    _spprof_InterpreterFrame *frame = _spprof_get_current_frame(tstate);
    
    int count = 0;
    int safety_limit = 500;  /* Prevent infinite loops from corruption */
    
    while (frame != NULL && count < max_frames && safety_limit-- > 0) {
        /* Validate frame pointer */
        if (!_spprof_ptr_valid(frame)) {
            break;
        }
        
        /* Get code object pointer */
        PyCodeObject *code = _spprof_frame_get_code(frame);
        if (_spprof_ptr_valid(code)) {
            frames[count++] = (uintptr_t)code;
        }
        
        /* Move to previous frame */
        frame = _spprof_frame_get_previous(frame);
    }
    
    return count;
}

/**
 * Capture Python frame code and instruction pointers - ASYNC-SIGNAL-SAFE
 *
 * This variant also captures instruction pointers for accurate line number
 * resolution in the resolver.
 *
 * @param code_ptrs Output array for code object pointers
 * @param instr_ptrs Output array for instruction pointers (parallel to code_ptrs)
 * @param max_frames Maximum number of frames to capture
 * @return Number of frames captured (0 to max_frames)
 */
static inline int
_spprof_capture_frames_with_instr_unsafe(
    uintptr_t *code_ptrs,
    uintptr_t *instr_ptrs,
    int max_frames
) {
    if (code_ptrs == NULL || instr_ptrs == NULL || max_frames <= 0) {
        return 0;
    }

    /* Get thread state from TLS */
    PyThreadState *tstate = _spprof_tstate_get();
    if (!_spprof_ptr_valid(tstate)) {
        return 0;
    }

    /* Get current interpreter frame */
    _spprof_InterpreterFrame *frame = _spprof_get_current_frame(tstate);
    
    int count = 0;
    int safety_limit = 500;  /* Prevent infinite loops from corruption */
    
    while (frame != NULL && count < max_frames && safety_limit-- > 0) {
        /* Validate frame pointer */
        if (!_spprof_ptr_valid(frame)) {
            break;
        }
        
        /* Skip shim frames (C-stack) */
        if (_spprof_frame_is_shim(frame)) {
            frame = _spprof_frame_get_previous(frame);
            continue;
        }
        
        /* Get code object pointer */
        PyCodeObject *code = _spprof_frame_get_code(frame);
        if (_spprof_ptr_valid(code)) {
            code_ptrs[count] = (uintptr_t)code;
            
            /* Get instruction pointer for line number resolution */
            _Py_CODEUNIT *instr = _spprof_frame_get_instr_ptr(frame);
            instr_ptrs[count] = _spprof_ptr_valid(instr) ? (uintptr_t)instr : 0;
            
            count++;
        }
        
        /* Move to previous frame */
        frame = _spprof_frame_get_previous(frame);
    }
    
    return count;
}

/**
 * Extended frame data for more precise profiling
 */
typedef struct {
    uintptr_t code_ptr;     /* PyCodeObject pointer */
    uintptr_t instr_ptr;    /* Instruction pointer within code */
    int8_t owner;           /* Frame owner type */
    int8_t padding[7];      /* Alignment */
} _spprof_FrameData;

/**
 * Capture frames with instruction pointers - ASYNC-SIGNAL-SAFE
 *
 * This variant captures both the code object and instruction pointer,
 * enabling more precise line number resolution.
 */
static inline int
_spprof_capture_frames_extended(
    _spprof_FrameData *frames,
    int max_frames
) {
    if (frames == NULL || max_frames <= 0) {
        return 0;
    }

    PyThreadState *tstate = _spprof_tstate_get();
    if (!_spprof_ptr_valid(tstate)) {
        return 0;
    }

    _spprof_InterpreterFrame *frame = _spprof_get_current_frame(tstate);
    
    int count = 0;
    int safety_limit = 500;
    
    while (frame != NULL && count < max_frames && safety_limit-- > 0) {
        if (!_spprof_ptr_valid(frame)) {
            break;
        }
        
        if (_spprof_frame_is_shim(frame)) {
            frame = _spprof_frame_get_previous(frame);
            continue;
        }
        
        PyCodeObject *code = _spprof_frame_get_code(frame);
        if (_spprof_ptr_valid(code)) {
            frames[count].code_ptr = (uintptr_t)code;
            frames[count].instr_ptr = (uintptr_t)_spprof_frame_get_instr_ptr(frame);
            frames[count].owner = _spprof_frame_get_owner(frame);
            count++;
        }
        
        frame = _spprof_frame_get_previous(frame);
    }
    
    return count;
}

/**
 * Get Python thread ID from thread state
 */
static inline uint64_t
_spprof_tstate_thread_id(PyThreadState *tstate) {
    if (!_spprof_ptr_valid(tstate)) return 0;
    return (uint64_t)tstate->thread_id;
}


#ifdef __cplusplus
}
#endif

#endif /* SPPROF_INTERNAL_PYCORE_TSTATE_H */
