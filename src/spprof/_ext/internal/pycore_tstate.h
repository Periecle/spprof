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
 * FREE-THREADING (Py_GIL_DISABLED) CONSIDERATIONS:
 *
 * Python 3.13+ can be built without the GIL (free-threaded builds).
 * This fundamentally changes the safety model:
 *
 *   GIL-ENABLED BUILDS:
 *     - Only one thread executes Python bytecode at a time
 *     - Signal handler interrupts the GIL-holding thread
 *     - Frame chain is stable (no concurrent modifications)
 *     - Sampling via SIGPROF is SAFE
 *
 *   FREE-THREADED BUILDS (Py_GIL_DISABLED):
 *     - Multiple threads execute Python bytecode simultaneously
 *     - Signal handler interrupts at arbitrary point
 *     - Frame chain may be modified by the interrupted thread itself
 *     - Functions calls/returns update frame->previous
 *     - Sampling via SIGPROF is NOT SAFE without additional measures
 *
 * SAFE APPROACHES FOR FREE-THREADED BUILDS:
 *   1. Thread Suspension (Mach/Darwin): Fully stops thread before reading
 *   2. Cooperative Sampling: Use PEP 669 callbacks at safe points
 *   3. Per-thread Locks: Not async-signal-safe, requires careful design
 *
 * Copyright (c) 2024 spprof contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef SPPROF_INTERNAL_PYCORE_TSTATE_H
#define SPPROF_INTERNAL_PYCORE_TSTATE_H

#include <Python.h>
#include <stdint.h>

/* C11 atomics - not available on older MSVC */
#ifndef _MSC_VER
#include <stdatomic.h>
#endif

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
 *
 * SIGNAL SAFETY CONSIDERATIONS:
 *
 * For async-signal-safe operation, we need a function that:
 *   1. Reads from TLS without locks
 *   2. Does not call malloc/free
 *   3. Does not acquire Python's GIL or any internal locks
 *
 * On Python 3.13+ with Py_GIL_DISABLED (free-threaded builds):
 *   - PyThreadState_Get() may acquire internal locks for thread-safety
 *   - This is NOT async-signal-safe
 *   - We use PyThreadState_GetUnchecked() instead which is a simple TLS read
 *
 * On regular (GIL-enabled) builds:
 *   - PyThreadState_Get()/PyThreadState_GET() are TLS reads and are safe
 *   - The GIL provides synchronization, so no additional locks needed
 */

/**
 * Get current thread state - ASYNC-SIGNAL-SAFE
 *
 * This reads from thread-local storage without locks or allocation.
 * On Linux with glibc, __thread variables are async-signal-safe.
 *
 * IMPORTANT: On free-threaded Python (Py_GIL_DISABLED), this uses
 * PyThreadState_GetUnchecked() which may return NULL if no thread state
 * is associated with the current thread. Callers must handle NULL.
 *
 * Version-specific behavior:
 *   - Python 3.9-3.12: PyThreadState_GET() is a direct TLS read (safe)
 *   - Python 3.13+: PyThreadState_GetUnchecked() avoids locks (safe)
 *
 * ASYNC-SIGNAL-SAFE: Direct TLS read on all versions.
 */
static inline PyThreadState*
_spprof_tstate_get(void) {
#if PY_VERSION_HEX >= 0x030D0000
    /*
     * Python 3.13+: Use PyThreadState_GetUnchecked() for signal safety.
     *
     * On free-threaded builds (Py_GIL_DISABLED), PyThreadState_Get() may
     * acquire internal locks which is NOT async-signal-safe. Using
     * GetUnchecked avoids this - it's a direct TLS read that returns NULL
     * if no thread state exists (rather than raising an exception).
     */
    return PyThreadState_GetUnchecked();
#else
    /*
     * Python 3.9-3.12: PyThreadState_GET() reads from _Py_tss_tstate
     * This is async-signal-safe as it's a direct TLS access.
     *
     * Note: PyThreadState_GET() is a macro that expands to a TLS read.
     */
    return PyThreadState_GET();
#endif
}

/*
 * =============================================================================
 * Architecture-Specific Atomic Loads (Free-Threading Support)
 * =============================================================================
 *
 * For free-threaded Python builds on Linux, we need architecture-appropriate
 * memory ordering for pointer reads:
 *
 *   x86-64: Strong memory model. All loads have implicit acquire semantics.
 *           Plain loads are sufficient - if we see a new pointer value, all
 *           prior writes by that thread are visible.
 *
 *   ARM64:  Weak memory model. Stores can be reordered. Without barriers,
 *           we might see a new pointer value but old data at that address.
 *           Use __atomic_load_n with __ATOMIC_ACQUIRE for frame pointer reads.
 */

#if defined(__aarch64__)
    /* ARM64: Weak memory model requires acquire barrier */
    #define SPPROF_ATOMIC_LOAD_PTR(ptr) \
        __atomic_load_n((void**)(ptr), __ATOMIC_ACQUIRE)
#else
    /* x86-64 and others: Strong memory model, plain load sufficient */
    #define SPPROF_ATOMIC_LOAD_PTR(ptr) (*(void**)(ptr))
#endif

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
 * Frame Walk Safety Limit
 * =============================================================================
 *
 * Maximum iterations when walking the frame chain.
 * This prevents infinite loops if memory corruption creates a cycle in the
 * frame->previous chain.
 *
 * Value of 500 is sufficient for the deepest reasonable Python call stacks
 * while still catching corruption quickly. For context:
 *   - Default Python recursion limit is 1000
 *   - We cap at SPPROF_MAX_STACK_DEPTH (128) frames per sample anyway
 *   - 500 gives ample headroom while bounding loop iterations
 *
 * Note: For thread state iteration, we use a higher limit (1000) since
 * processes may have more threads than stack depth.
 */
#define SPPROF_FRAME_WALK_LIMIT 500
#define SPPROF_THREAD_WALK_LIMIT 1000

/*
 * =============================================================================
 * Frame Stack Capture
 * =============================================================================
 *
 * FREE-THREADING SAFETY WARNING (Py_GIL_DISABLED):
 *
 * The functions below walk the frame chain by reading frame->previous
 * pointers. In GIL-enabled builds, this is safe because:
 *   - The GIL ensures only one thread modifies frames at a time
 *   - Signal handlers interrupt the GIL holder, who is paused
 *
 * In FREE-THREADED builds, this is UNSAFE for signal handlers because:
 *   - The interrupted thread could be in the middle of a call/return
 *   - frame->previous could be partially updated
 *   - We could read a half-written pointer → crash
 *
 * SAFE USAGE IN FREE-THREADED BUILDS:
 *   1. Use _spprof_capture_frames_from_tstate() with a SUSPENDED thread
 *   2. The Mach sampler (Darwin) does this via thread_suspend()
 *   3. Do NOT use _spprof_capture_frames_unsafe() in signal handlers
 *      on free-threaded builds
 *
 * The module.c startup code checks SPPROF_FREE_THREADING_SAFE and
 * disables signal-based profiling on unsafe configurations.
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
 * FREE-THREADING WARNING:
 *   This function is NOT SAFE to call from signal handlers in free-threaded
 *   builds (Py_GIL_DISABLED). The target thread could be modifying its
 *   frame chain concurrently. Use thread suspension (Mach sampler) instead.
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

#if SPPROF_FREE_THREADED && !defined(__APPLE__)
    /*
     * SAFETY CHECK: On free-threaded builds with signal-based sampling,
     * frame walking is unsafe. Return 0 frames.
     *
     * This should never be reached if module.c properly blocks startup,
     * but we add this as a defense-in-depth measure.
     */
    return 0;
#endif

    /* Get thread state from TLS */
    PyThreadState *tstate = _spprof_tstate_get();
    if (!_spprof_ptr_valid(tstate)) {
        return 0;
    }

    int count = 0;
    int safety_limit = SPPROF_FRAME_WALK_LIMIT;

#if SPPROF_PY39 || SPPROF_PY310
    /*
     * Python 3.9/3.10: Use PyFrameObject via tstate->frame
     *
     * ASYNC-SIGNAL-SAFE: Direct struct access, no Python API calls.
     */
    _spprof_PyFrameObject *frame = _spprof_get_current_frame(tstate);
    
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
#else
    /*
     * Python 3.11+: Use _PyInterpreterFrame via cframe or current_frame
     *
     * ASYNC-SIGNAL-SAFE: Direct struct access, no Python API calls
     * (except PyCode_Check on 3.13+ which is unavoidable).
     */
    _spprof_InterpreterFrame *frame = _spprof_get_current_frame(tstate);
    
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
#endif
    
    return count;
}

/**
 * Capture Python frame code and instruction pointers - ASYNC-SIGNAL-SAFE
 *
 * This variant also captures instruction pointers for accurate line number
 * resolution in the resolver.
 *
 * FREE-THREADING WARNING:
 *   This function is NOT SAFE to call from signal handlers in free-threaded
 *   builds (Py_GIL_DISABLED). See _spprof_capture_frames_unsafe() for details.
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

#if SPPROF_FREE_THREADED && !defined(__APPLE__)
    /*
     * SAFETY CHECK: On free-threaded builds with signal-based sampling,
     * frame walking is unsafe. Return 0 frames.
     */
    return 0;
#endif

    /* Get thread state from TLS */
    PyThreadState *tstate = _spprof_tstate_get();
    if (!_spprof_ptr_valid(tstate)) {
        return 0;
    }

    int count = 0;
    int safety_limit = SPPROF_FRAME_WALK_LIMIT;

#if SPPROF_PY39 || SPPROF_PY310
    /*
     * Python 3.9/3.10: Use PyFrameObject via tstate->frame
     *
     * Note: Python 3.9/3.10 don't have shim frames, so no skip logic needed.
     *
     * ASYNC-SIGNAL-SAFE: Direct struct access, no Python API calls.
     */
    _spprof_PyFrameObject *frame = _spprof_get_current_frame(tstate);
    
    while (frame != NULL && count < max_frames && safety_limit-- > 0) {
        /* Validate frame pointer */
        if (!_spprof_ptr_valid(frame)) {
            break;
        }
        
        /* Get code object pointer */
        PyCodeObject *code = _spprof_frame_get_code(frame);
        if (_spprof_ptr_valid(code)) {
            code_ptrs[count] = (uintptr_t)code;
            
            /* Get instruction pointer for line number resolution */
            void *instr = _spprof_frame_get_instr_ptr(frame);
            instr_ptrs[count] = _spprof_ptr_valid(instr) ? (uintptr_t)instr : 0;
            
            count++;
        }
        
        /* Move to previous frame */
        frame = _spprof_frame_get_previous(frame);
    }
#else
    /*
     * Python 3.11+: Use _PyInterpreterFrame via cframe or current_frame
     *
     * ASYNC-SIGNAL-SAFE: Direct struct access, no Python API calls
     * (except PyCode_Check on 3.13+ which is unavoidable).
     */
    _spprof_InterpreterFrame *frame = _spprof_get_current_frame(tstate);
    
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
            void *instr = _spprof_frame_get_instr_ptr(frame);
            instr_ptrs[count] = _spprof_ptr_valid(instr) ? (uintptr_t)instr : 0;
            
            count++;
        }
        
        /* Move to previous frame */
        frame = _spprof_frame_get_previous(frame);
    }
#endif
    
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
 *
 * ASYNC-SIGNAL-SAFE: Direct struct access, no Python API calls
 * (except PyCode_Check on 3.13+ which is unavoidable).
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

    int count = 0;
    int safety_limit = SPPROF_FRAME_WALK_LIMIT;

#if SPPROF_PY39 || SPPROF_PY310
    /*
     * Python 3.9/3.10: Use PyFrameObject via tstate->frame
     */
    _spprof_PyFrameObject *frame = _spprof_get_current_frame(tstate);
    
    while (frame != NULL && count < max_frames && safety_limit-- > 0) {
        if (!_spprof_ptr_valid(frame)) {
            break;
        }
        
        /* Python 3.9/3.10 don't have shim frames, no skip needed */
        
        PyCodeObject *code = _spprof_frame_get_code(frame);
        if (_spprof_ptr_valid(code)) {
            frames[count].code_ptr = (uintptr_t)code;
            frames[count].instr_ptr = (uintptr_t)_spprof_frame_get_instr_ptr(frame);
            frames[count].owner = _spprof_frame_get_owner(frame);
            count++;
        }
        
        frame = _spprof_frame_get_previous(frame);
    }
#else
    /*
     * Python 3.11+: Use _PyInterpreterFrame
     */
    _spprof_InterpreterFrame *frame = _spprof_get_current_frame(tstate);
    
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
#endif
    
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

/*
 * =============================================================================
 * Thread State Lookup by Thread ID
 * =============================================================================
 *
 * These functions enable finding a PyThreadState for a specific OS thread ID.
 * Used by Mach-based sampling where we suspend a thread and need to find its
 * Python state.
 *
 * NOTE: These are NOT async-signal-safe but are safe to call from the
 * Mach sampler thread (which is not a signal handler).
 *
 * FREE-THREADING CONSIDERATIONS:
 *
 * In free-threaded builds, iterating the thread state list requires care:
 *
 *   1. PyInterpreterState_ThreadHead/PyThreadState_Next:
 *      - In GIL-enabled builds: Safe with GIL held
 *      - In free-threaded builds: Use critical sections or ensure GIL
 *
 *   2. The darwin_mach.c sampler acquires the GIL via PyGILState_Ensure()
 *      before calling these functions, which provides the necessary
 *      synchronization even in free-threaded builds.
 *
 *   3. In free-threaded builds, GIL acquisition still works - it just
 *      acquires the runtime critical section instead.
 */

/**
 * Find PyThreadState by OS thread ID.
 *
 * Iterates through all thread states in all interpreters to find the one
 * matching the given thread ID.
 *
 * Thread safety: NOT async-signal-safe. Safe to call from sampler thread.
 *                Requires GIL (or critical section in free-threaded builds).
 * 
 * @param thread_id OS thread ID to find
 * @return PyThreadState* or NULL if not found
 */
static inline PyThreadState*
_spprof_find_tstate_by_thread_id(uint64_t thread_id) {
    /* Get main interpreter */
    PyInterpreterState *interp = PyInterpreterState_Main();
    if (!_spprof_ptr_valid(interp)) {
        return NULL;
    }
    
    /* Iterate through all thread states.
     * REQUIRES GIL or critical section for thread safety.
     * The caller (darwin_mach.c) ensures this via PyGILState_Ensure(). */
    PyThreadState *tstate = PyInterpreterState_ThreadHead(interp);
    int safety_limit = SPPROF_THREAD_WALK_LIMIT;
    
    while (tstate != NULL && safety_limit-- > 0) {
        if (!_spprof_ptr_valid(tstate)) {
            break;
        }
        
        if ((uint64_t)tstate->thread_id == thread_id) {
            return tstate;
        }
        
        tstate = PyThreadState_Next(tstate);
    }
    
    return NULL;
}

/**
 * Capture Python frames from a specific PyThreadState (not current thread).
 *
 * This is used by Mach-based sampling where we've suspended a thread and
 * want to capture its frames without using TLS.
 *
 * SAFETY: Call only when the target thread is suspended. The thread's frame
 * chain must be stable during capture.
 *
 * FREE-THREADING SAFETY:
 *   This function IS SAFE for free-threaded builds (Py_GIL_DISABLED) when
 *   the target thread is suspended via thread_suspend() (Mach sampler).
 *   Thread suspension guarantees frame chain stability regardless of GIL.
 *
 * @param tstate PyThreadState to capture frames from
 * @param frames Output array for code object pointers
 * @param max_frames Maximum number of frames to capture
 * @return Number of frames captured (0 to max_frames)
 */
static inline int
_spprof_capture_frames_from_tstate(
    PyThreadState *tstate,
    uintptr_t *frames,
    int max_frames
) {
    if (frames == NULL || max_frames <= 0) {
        return 0;
    }
    
    if (!_spprof_ptr_valid(tstate)) {
        return 0;
    }
    
    int count = 0;
    int safety_limit = SPPROF_FRAME_WALK_LIMIT;

#if SPPROF_PY39 || SPPROF_PY310
    /*
     * Python 3.9/3.10: Use PyFrameObject via tstate->frame
     */
    _spprof_PyFrameObject *frame = _spprof_get_current_frame(tstate);
    
    while (frame != NULL && count < max_frames && safety_limit-- > 0) {
        /* Validate frame pointer */
        if (!_spprof_ptr_valid(frame)) {
            break;
        }
        
        /* Python 3.9/3.10 don't have shim frames */
        
        /* Get code object pointer */
        PyCodeObject *code = _spprof_frame_get_code(frame);
        if (_spprof_ptr_valid(code)) {
            frames[count++] = (uintptr_t)code;
        }
        
        /* Move to previous frame */
        frame = _spprof_frame_get_previous(frame);
    }
#else
    /*
     * Python 3.11+: Use _PyInterpreterFrame
     */
    _spprof_InterpreterFrame *frame = _spprof_get_current_frame(tstate);
    
    while (frame != NULL && count < max_frames && safety_limit-- > 0) {
        /* Validate frame pointer */
        if (!_spprof_ptr_valid(frame)) {
            break;
        }
        
        /* Skip shim frames (C-stack owned) */
        if (_spprof_frame_is_shim(frame)) {
            frame = _spprof_frame_get_previous(frame);
            continue;
        }
        
        /* Get code object pointer */
        PyCodeObject *code = _spprof_frame_get_code(frame);
        if (_spprof_ptr_valid(code)) {
            frames[count++] = (uintptr_t)code;
        }
        
        /* Move to previous frame */
        frame = _spprof_frame_get_previous(frame);
    }
#endif
    
    return count;
}

/**
 * Capture Python frames with instruction pointers from a specific PyThreadState.
 *
 * This variant also captures instruction pointers for accurate line number
 * resolution.
 *
 * FREE-THREADING SAFETY:
 *   This function IS SAFE for free-threaded builds (Py_GIL_DISABLED) when
 *   the target thread is suspended via thread_suspend() (Mach sampler).
 *   Thread suspension guarantees frame chain stability regardless of GIL.
 *
 * @param tstate PyThreadState to capture frames from
 * @param code_ptrs Output array for code object pointers
 * @param instr_ptrs Output array for instruction pointers
 * @param max_frames Maximum number of frames to capture
 * @return Number of frames captured (0 to max_frames)
 */
static inline int
_spprof_capture_frames_with_instr_from_tstate(
    PyThreadState *tstate,
    uintptr_t *code_ptrs,
    uintptr_t *instr_ptrs,
    int max_frames
) {
    if (code_ptrs == NULL || instr_ptrs == NULL || max_frames <= 0) {
        return 0;
    }
    
    if (!_spprof_ptr_valid(tstate)) {
        return 0;
    }
    
    int count = 0;
    int safety_limit = SPPROF_FRAME_WALK_LIMIT;

#if SPPROF_PY39 || SPPROF_PY310
    /*
     * Python 3.9/3.10: Use PyFrameObject via tstate->frame
     */
    _spprof_PyFrameObject *frame = _spprof_get_current_frame(tstate);
    
    while (frame != NULL && count < max_frames && safety_limit-- > 0) {
        if (!_spprof_ptr_valid(frame)) {
            break;
        }
        
        /* Python 3.9/3.10 don't have shim frames */
        
        /* Get code object pointer */
        PyCodeObject *code = _spprof_frame_get_code(frame);
        if (_spprof_ptr_valid(code)) {
            code_ptrs[count] = (uintptr_t)code;
            
            /* Get instruction pointer for line number resolution */
            void *instr = _spprof_frame_get_instr_ptr(frame);
            instr_ptrs[count] = _spprof_ptr_valid(instr) ? (uintptr_t)instr : 0;
            
            count++;
        }
        
        frame = _spprof_frame_get_previous(frame);
    }
#else
    /*
     * Python 3.11+: Use _PyInterpreterFrame
     */
    _spprof_InterpreterFrame *frame = _spprof_get_current_frame(tstate);
    
    while (frame != NULL && count < max_frames && safety_limit-- > 0) {
        if (!_spprof_ptr_valid(frame)) {
            break;
        }
        
        /* Skip shim frames */
        if (_spprof_frame_is_shim(frame)) {
            frame = _spprof_frame_get_previous(frame);
            continue;
        }
        
        /* Get code object pointer */
        PyCodeObject *code = _spprof_frame_get_code(frame);
        if (_spprof_ptr_valid(code)) {
            code_ptrs[count] = (uintptr_t)code;
            
            /* Get instruction pointer for line number resolution */
            void *instr = _spprof_frame_get_instr_ptr(frame);
            instr_ptrs[count] = _spprof_ptr_valid(instr) ? (uintptr_t)instr : 0;
            
            count++;
        }
        
        frame = _spprof_frame_get_previous(frame);
    }
#endif
    
    return count;
}

/*
 * =============================================================================
 * Speculative Frame Capture (Free-Threading Safe - Linux)
 * =============================================================================
 *
 * These functions implement speculative frame reading with validation for
 * free-threaded Python builds on Linux. They are designed to be async-signal-safe.
 *
 * KEY DESIGN PRINCIPLES:
 *   1. Speculative reads: Read pointers without synchronization
 *   2. Multi-layer validation: Check bounds, alignment, type before use
 *   3. Cycle detection: Prevent infinite loops from corruption
 *   4. Graceful degradation: Drop corrupted samples rather than crashing
 *
 * SAFETY MODEL:
 *   On x86-64/ARM64, aligned pointer reads/writes are atomic at hardware level.
 *   The danger is reading stale or freed memory. Race window during frame chain
 *   updates is ~10-50ns, sampling interval is 10ms → ~0.0005% chance per sample.
 *
 * MEMORY ORDERING:
 *   x86-64: Strong model, plain loads sufficient
 *   ARM64: Weak model, use SPPROF_ATOMIC_LOAD_PTR for acquire semantics
 */

#if SPPROF_FREE_THREADED && defined(__linux__)

/* External declarations for globals in signal_handler.c */
extern PyTypeObject *_spprof_cached_code_type;
extern int _spprof_speculative_initialized;
extern _Atomic uint64_t _spprof_samples_dropped_validation;

/* Heap bounds for pointer validation */
#define SPPROF_HEAP_LOWER_BOUND  ((uintptr_t)0x10000)

#if defined(__aarch64__)
    /* ARM64: 48-bit user-space address limit */
    #define SPPROF_HEAP_UPPER_BOUND  ((uintptr_t)0x0000FFFFFFFFFFFF)
#else
    /* x86-64: 47-bit user-space address limit */
    #define SPPROF_HEAP_UPPER_BOUND  ((uintptr_t)0x00007FFFFFFFFFFF)
#endif

/* Cycle detection window size (fits in cache line) */
#define SPPROF_CYCLE_WINDOW_SIZE 8

/**
 * Initialize speculative capture validation state.
 *
 * MUST be called during module initialization (with GIL held).
 * Caches PyCode_Type pointer for async-signal-safe type checking.
 *
 * Thread Safety: NOT thread-safe. Call once during init.
 * Signal Safety: NOT async-signal-safe. Do not call from handler.
 *
 * @return 0 on success
 */
static inline int _spprof_speculative_init(void) {
    _spprof_cached_code_type = &PyCode_Type;
    _spprof_speculative_initialized = 1;
    return 0;
}

/**
 * Enhanced pointer validation for speculative capture - ASYNC-SIGNAL-SAFE
 *
 * Performs more thorough validation than _spprof_ptr_valid():
 *   1. NULL check
 *   2. Heap bounds check (architecture-specific)
 *   3. 8-byte alignment check (all Python objects are aligned)
 *
 * @param ptr Pointer to validate
 * @return 1 if valid, 0 if invalid
 */
static inline int _spprof_ptr_valid_speculative(const void *ptr) {
    uintptr_t addr = (uintptr_t)ptr;
    return addr >= SPPROF_HEAP_LOWER_BOUND
        && addr <= SPPROF_HEAP_UPPER_BOUND
        && (addr & 0x7) == 0;
}

/**
 * Check if object looks like a PyCodeObject - ASYNC-SIGNAL-SAFE
 *
 * Compares ob_type to cached PyCode_Type pointer without calling Python API.
 * This is safe because:
 *   - PyCode_Type is a static global in Python runtime (never freed)
 *   - Address is constant for interpreter lifetime
 *   - Comparison is just pointer equality
 *
 * @param obj Object to check (must have passed ptr_valid_speculative)
 * @return 1 if looks like code object, 0 otherwise
 */
static inline int _spprof_looks_like_code(PyObject *obj) {
    if (!_spprof_ptr_valid_speculative(obj)) return 0;
    PyTypeObject *type = obj->ob_type;
    if (!_spprof_ptr_valid_speculative(type)) return 0;
    return type == _spprof_cached_code_type;
}

/**
 * Capture Python frames speculatively with validation - ASYNC-SIGNAL-SAFE
 *
 * For use in signal handlers on free-threaded Python builds.
 * Validates each pointer before dereferencing and detects cycles.
 *
 * @param frames Output array for code object pointers
 * @param max_frames Maximum frames to capture (must be <= SPPROF_MAX_STACK_DEPTH)
 * @return Number of valid frames captured, or 0 if validation failed
 */
static inline int
_spprof_capture_frames_speculative(uintptr_t *frames, int max_frames) {
    if (!_spprof_speculative_initialized || frames == NULL || max_frames <= 0) {
        return 0;
    }

    /* Get thread state from TLS */
    PyThreadState *tstate = _spprof_tstate_get();
    if (!_spprof_ptr_valid_speculative(tstate)) {
        return 0;
    }

    int depth = 0;
    uintptr_t seen[SPPROF_CYCLE_WINDOW_SIZE] = {0};
    int seen_idx = 0;
    int safety_limit = SPPROF_FRAME_WALK_LIMIT;

    /* Get current frame with appropriate memory ordering */
    _spprof_InterpreterFrame *frame =
        (_spprof_InterpreterFrame *)SPPROF_ATOMIC_LOAD_PTR(&tstate->current_frame);

    while (depth < max_frames && safety_limit-- > 0) {
        /* 1. Validate frame pointer */
        if (!_spprof_ptr_valid_speculative(frame)) {
            break;
        }

        /* 2. Cycle detection - check against recent frames */
        for (int i = 0; i < SPPROF_CYCLE_WINDOW_SIZE && i < depth; i++) {
            if (seen[i] == (uintptr_t)frame) {
                /* Cycle detected - drop sample and increment counter */
                atomic_fetch_add_explicit(
                    &_spprof_samples_dropped_validation, 1,
                    memory_order_relaxed);
                return 0;
            }
        }
        /* Record this frame in rolling window */
        seen[seen_idx++ & (SPPROF_CYCLE_WINDOW_SIZE - 1)] = (uintptr_t)frame;

        /* 3. Skip shim frames (owned by C stack) */
        if (frame->owner == SPPROF_FRAME_OWNED_BY_CSTACK) {
            frame = (_spprof_InterpreterFrame *)
                SPPROF_ATOMIC_LOAD_PTR(&frame->previous);
            continue;
        }

        /* 4. Extract code object (handle tagged pointers for Python 3.14) */
#if SPPROF_PY314
        PyObject *code = (PyObject *)(frame->f_executable.bits & ~SPPROF_STACKREF_TAG_MASK);
#else
        PyObject *code = frame->f_executable;
#endif

        /* 5. Validate code object using cached type pointer */
        if (_spprof_looks_like_code(code)) {
            frames[depth++] = (uintptr_t)code;
        }

        /* 6. Move to previous frame with memory ordering */
        frame = (_spprof_InterpreterFrame *)
            SPPROF_ATOMIC_LOAD_PTR(&frame->previous);
    }

    return depth;
}

/**
 * Capture Python frames with instruction pointers, speculatively - ASYNC-SIGNAL-SAFE
 *
 * Same as _spprof_capture_frames_speculative but also captures instruction
 * pointers for accurate line number resolution.
 *
 * @param code_ptrs Output array for code object pointers
 * @param instr_ptrs Output array for instruction pointers (parallel)
 * @param max_frames Maximum frames to capture
 * @return Number of valid frames captured
 */
static inline int
_spprof_capture_frames_with_instr_speculative(
    uintptr_t *code_ptrs,
    uintptr_t *instr_ptrs,
    int max_frames
) {
    if (!_spprof_speculative_initialized || code_ptrs == NULL ||
        instr_ptrs == NULL || max_frames <= 0) {
        return 0;
    }

    /* Get thread state from TLS */
    PyThreadState *tstate = _spprof_tstate_get();
    if (!_spprof_ptr_valid_speculative(tstate)) {
        return 0;
    }

    int depth = 0;
    uintptr_t seen[SPPROF_CYCLE_WINDOW_SIZE] = {0};
    int seen_idx = 0;
    int safety_limit = SPPROF_FRAME_WALK_LIMIT;

    /* Get current frame with appropriate memory ordering */
    _spprof_InterpreterFrame *frame =
        (_spprof_InterpreterFrame *)SPPROF_ATOMIC_LOAD_PTR(&tstate->current_frame);

    while (depth < max_frames && safety_limit-- > 0) {
        /* 1. Validate frame pointer */
        if (!_spprof_ptr_valid_speculative(frame)) {
            break;
        }

        /* 2. Cycle detection */
        for (int i = 0; i < SPPROF_CYCLE_WINDOW_SIZE && i < depth; i++) {
            if (seen[i] == (uintptr_t)frame) {
                atomic_fetch_add_explicit(
                    &_spprof_samples_dropped_validation, 1,
                    memory_order_relaxed);
                return 0;
            }
        }
        seen[seen_idx++ & (SPPROF_CYCLE_WINDOW_SIZE - 1)] = (uintptr_t)frame;

        /* 3. Skip shim frames */
        if (frame->owner == SPPROF_FRAME_OWNED_BY_CSTACK) {
            frame = (_spprof_InterpreterFrame *)
                SPPROF_ATOMIC_LOAD_PTR(&frame->previous);
            continue;
        }

        /* 4. Extract code object */
#if SPPROF_PY314
        PyObject *code = (PyObject *)(frame->f_executable.bits & ~SPPROF_STACKREF_TAG_MASK);
#else
        PyObject *code = frame->f_executable;
#endif

        /* 5. Validate and store code object and instruction pointer */
        if (_spprof_looks_like_code(code)) {
            code_ptrs[depth] = (uintptr_t)code;

            /* Get instruction pointer for line number resolution */
            void *instr = _spprof_frame_get_instr_ptr(frame);
            instr_ptrs[depth] = _spprof_ptr_valid_speculative(instr)
                ? (uintptr_t)instr : 0;

            depth++;
        }

        /* 6. Move to previous frame */
        frame = (_spprof_InterpreterFrame *)
            SPPROF_ATOMIC_LOAD_PTR(&frame->previous);
    }

    return depth;
}

#endif /* SPPROF_FREE_THREADED && __linux__ */

#ifdef __cplusplus
}
#endif

#endif /* SPPROF_INTERNAL_PYCORE_TSTATE_H */
