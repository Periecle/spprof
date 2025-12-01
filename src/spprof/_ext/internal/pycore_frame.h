/**
 * internal/pycore_frame.h - Python internal frame structures
 *
 * This header defines Python's internal frame structures for async-signal-safe
 * frame walking. These structures mirror CPython's internal layout and must be
 * kept in sync with the Python version being built against.
 *
 * CRITICAL: This code is version-specific. The struct layouts MUST match
 * what CPython uses internally, or memory access will be incorrect.
 *
 * Supported versions:
 *   - Python 3.11.x
 *   - Python 3.12.x
 *   - Python 3.13.x
 *   - Python 3.14.x
 *
 * References:
 *   - CPython Include/internal/pycore_frame.h
 *   - CPython Include/cpython/pystate.h
 *
 * Copyright (c) 2024 spprof contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef SPPROF_INTERNAL_PYCORE_FRAME_H
#define SPPROF_INTERNAL_PYCORE_FRAME_H

#include <Python.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * =============================================================================
 * Version Detection
 * =============================================================================
 */

#define SPPROF_PY311 (PY_VERSION_HEX >= 0x030B0000 && PY_VERSION_HEX < 0x030C0000)
#define SPPROF_PY312 (PY_VERSION_HEX >= 0x030C0000 && PY_VERSION_HEX < 0x030D0000)
#define SPPROF_PY313 (PY_VERSION_HEX >= 0x030D0000 && PY_VERSION_HEX < 0x030E0000)
#define SPPROF_PY314 (PY_VERSION_HEX >= 0x030E0000)

#if PY_VERSION_HEX < 0x030B0000
    #error "spprof internal API requires Python 3.11 or later"
#endif

/*
 * =============================================================================
 * Free-Threading Detection (Python 3.13+ with Py_GIL_DISABLED)
 * =============================================================================
 *
 * Python 3.13+ can be built with free-threading support (--disable-gil).
 * In these builds, Py_GIL_DISABLED is defined and the GIL is removed.
 *
 * CRITICAL SAFETY IMPLICATIONS:
 *
 * 1. Without the GIL, multiple threads can execute Python bytecode simultaneously.
 *    This means frame chains can be modified by the target thread while we're
 *    walking them.
 *
 * 2. SIGNAL-BASED SAMPLING (Linux SIGPROF):
 *    - NOT SAFE for free-threading without additional synchronization
 *    - The target thread can be anywhere when interrupted by signal
 *    - Frame chain could be in inconsistent state during function entry/exit
 *    - Solution: Use per-thread critical sections or disable until safe
 *
 * 3. MACH-BASED SAMPLING (Darwin/macOS):
 *    - SAFE for free-threading because we use thread_suspend()
 *    - Target thread is fully stopped before we read its state
 *    - Frame chain is stable during capture
 *
 * 4. Reference counting:
 *    - Py_INCREF/Py_DECREF need critical sections in free-threaded builds
 *    - Or use deferred reference counting APIs when available
 *
 * For now, we detect free-threaded builds and:
 *    - Darwin/Mach: Allow profiling (thread suspension is safe)
 *    - Linux/SIGPROF: Disable profiling with clear error message
 */

/* Detect free-threaded builds */
#ifdef Py_GIL_DISABLED
    #define SPPROF_FREE_THREADED 1
#else
    #define SPPROF_FREE_THREADED 0
#endif

/* Check if current platform's sampling method is safe for free-threading */
#if SPPROF_FREE_THREADED
    #if defined(__APPLE__)
        /* Mach sampler uses thread_suspend - safe for free-threading */
        #define SPPROF_FREE_THREADING_SAFE 1
    #else
        /* Signal-based sampling is NOT safe for free-threading */
        #define SPPROF_FREE_THREADING_SAFE 0
    #endif
#else
    /* GIL-enabled builds are always safe */
    #define SPPROF_FREE_THREADING_SAFE 1
#endif

/*
 * =============================================================================
 * Internal Types (not exposed in public headers for 3.13+)
 * =============================================================================
 */

#if SPPROF_PY313 || SPPROF_PY314
/*
 * _Py_CODEUNIT is an internal type that's not available in public headers
 * for Python 3.13+. We define our own compatible version.
 * Each instruction is a fixed-width 2-byte value: 1-byte opcode + 1-byte oparg
 */
typedef union _spprof_CODEUNIT {
    uint16_t cache;
    struct {
        uint8_t code;
        uint8_t arg;
    } op;
} _spprof_CODEUNIT;

/* Use our own type alias */
#define _Py_CODEUNIT _spprof_CODEUNIT

/*
 * _PyStackRef is used in Python 3.14 for tagged pointers.
 *
 * CPYTHON INTERNAL TAGGING SCHEME (Python 3.14):
 *
 * In free-threaded builds (Py_GIL_DISABLED), the low bits of the pointer
 * are used for reference counting metadata:
 *
 *   Bit 0: Deferred reference flag (PyStackRef_IsDeferredReference)
 *          - If set, this is a "deferred" reference that doesn't own a refcount
 *          - Used for borrowed references in the interpreter
 *
 *   Bit 1: Reserved / implementation-defined
 *          - CPython reserves this for future use
 *          - We mask it out for safety
 *
 * In GIL-enabled builds, the pointer is typically untagged (bits = raw pointer).
 *
 * VERSION COMPATIBILITY NOTE:
 *   This tagging scheme is specific to CPython 3.14's implementation.
 *   Future Python versions may change the tagging scheme. If this happens:
 *   1. Update the SPPROF_STACKREF_TAG_MASK below
 *   2. Test thoroughly with the new Python version
 *   3. Consider adding version-specific code paths
 *
 * Reference: CPython Include/internal/pycore_stackref.h
 */
typedef union _spprof_StackRef {
    uintptr_t bits;
} _spprof_StackRef;

/*
 * Tag bit mask for _PyStackRef pointers.
 * Masks out the lowest 2 bits to extract the actual PyObject* pointer.
 *
 * This value is based on CPython 3.14's implementation where:
 *   - Bit 0: Deferred reference flag
 *   - Bit 1: Reserved
 *
 * If future Python versions use different tagging, update this mask.
 */
#define SPPROF_STACKREF_TAG_MASK ((uintptr_t)0x3)

#endif /* SPPROF_PY313 || SPPROF_PY314 */

/*
 * Frame ownership constants - consistent across all versions
 */
enum _spprof_frameowner {
    SPPROF_FRAME_OWNED_BY_THREAD = 0,
    SPPROF_FRAME_OWNED_BY_GENERATOR = 1,
    SPPROF_FRAME_OWNED_BY_FRAME_OBJECT = 2,
    SPPROF_FRAME_OWNED_BY_CSTACK = 3,
};

/*
 * =============================================================================
 * Python 3.11 Frame Structures
 * =============================================================================
 *
 * In 3.11, _PyInterpreterFrame is the internal frame representation.
 * PyFrameObject wraps it for user-visible frames.
 *
 * Thread state has: cframe -> current_frame -> previous
 *
 * IMPORTANT: Field order MUST match CPython's pycore_frame.h exactly!
 */
#if SPPROF_PY311

typedef struct _spprof_InterpreterFrame {
    /* "Specials" section - order MUST match CPython 3.11 */
    PyFunctionObject *f_func;    /* Strong reference (offset 0) */
    PyObject *f_globals;         /* Borrowed reference (offset 8) */
    PyObject *f_builtins;        /* Borrowed reference (offset 16) */
    PyObject *f_locals;          /* Strong reference, may be NULL (offset 24) */
    PyCodeObject *f_code;        /* Strong reference (offset 32) */
    PyFrameObject *frame_obj;    /* Strong reference, may be NULL (offset 40) */
    /* Linkage section */
    struct _spprof_InterpreterFrame *previous;  /* (offset 48) */
    _Py_CODEUNIT *prev_instr;    /* Last executed instruction (offset 56) */
    int stacktop;                /* Offset of TOS from localsplus (offset 64) */
    _Bool is_entry;              /* Whether this is the "root" frame (offset 68) */
    char owner;                  /* enum _frameowner (offset 69) */
    /* Followed by localsplus array */
} _spprof_InterpreterFrame;

/* 
 * CFrame links C and Python frames in 3.11
 * IMPORTANT: Field order MUST match CPython's cpython/pystate.h exactly!
 */
typedef struct _spprof_CFrame {
    uint8_t use_tracing;         /* 0 or 255 (offset 0) */
    /* 7 bytes padding for alignment */
    _spprof_InterpreterFrame *current_frame;  /* (offset 8) */
    struct _spprof_CFrame *previous;          /* (offset 16) */
} _spprof_CFrame;

static inline _spprof_InterpreterFrame*
_spprof_get_current_frame(PyThreadState *tstate) {
    if (tstate == NULL) return NULL;
    /* In 3.11, tstate->cframe->current_frame */
    _spprof_CFrame *cf = (_spprof_CFrame *)tstate->cframe;
    return cf ? cf->current_frame : NULL;
}

static inline PyCodeObject*
_spprof_frame_get_code(_spprof_InterpreterFrame *frame) {
    return frame ? frame->f_code : NULL;
}

#endif /* SPPROF_PY311 */

/*
 * =============================================================================
 * Python 3.12 Frame Structures
 * =============================================================================
 *
 * Major changes in 3.12:
 *   - CFrame removed, current_frame directly in tstate
 *   - prev_instr renamed to instr_ptr
 *   - f_code renamed to f_executable in some contexts
 */
#if SPPROF_PY312

/*
 * Python 3.12 _PyInterpreterFrame - EXACT layout from CPython 3.12 source
 * From Include/internal/pycore_frame.h
 */
typedef struct _spprof_InterpreterFrame {
    PyCodeObject *f_code;        /* Strong reference - first field! */
    struct _spprof_InterpreterFrame *previous;
    PyObject *f_funcobj;
    PyObject *f_globals;
    PyObject *f_builtins;
    PyObject *f_locals;
    PyFrameObject *frame_obj;
    _Py_CODEUNIT *prev_instr;    /* NOT instr_ptr - named prev_instr in 3.12 */
    int stacktop;
    uint16_t return_offset;
    char owner;
    /* Followed by localsplus array */
} _spprof_InterpreterFrame;

/*
 * _PyCFrame structure (still exists in 3.12)
 */
typedef struct _spprof_CFrame {
    struct _spprof_InterpreterFrame *current_frame;
    struct _spprof_CFrame *previous;
} _spprof_CFrame;

static inline _spprof_InterpreterFrame*
_spprof_get_current_frame(PyThreadState *tstate) {
    if (tstate == NULL) return NULL;
    /* Python 3.12 uses cframe->current_frame */
    _spprof_CFrame *cf = (_spprof_CFrame *)tstate->cframe;
    return cf ? cf->current_frame : NULL;
}

static inline PyCodeObject*
_spprof_frame_get_code(_spprof_InterpreterFrame *frame) {
    if (frame == NULL) return NULL;
    /* In 3.12, f_code is directly a PyCodeObject* */
    return frame->f_code;
}

#endif /* SPPROF_PY312 */

/*
 * =============================================================================
 * Python 3.13 Frame Structures
 * =============================================================================
 *
 * 3.13 changes:
 *   - f_executable replaces f_code (more general)
 *   - Free-threading support (optional)
 *   - current_frame is directly in tstate (no more cframe)
 */
#if SPPROF_PY313

typedef struct _spprof_InterpreterFrame {
    PyObject *f_executable;      /* Strong reference (code object or None) */
    struct _spprof_InterpreterFrame *previous;
    PyObject *f_funcobj;         /* Strong reference */
    PyObject *f_globals;         /* Borrowed reference */
    PyObject *f_builtins;        /* Borrowed reference */
    PyObject *f_locals;          /* Strong reference, may be NULL */
    PyFrameObject *frame_obj;    /* Strong reference, may be NULL */
    _Py_CODEUNIT *instr_ptr;     /* Instruction currently executing */
    int stacktop;                /* Offset of TOS from localsplus */
    uint16_t return_offset;
    char owner;
    /* Followed by localsplus array */
} _spprof_InterpreterFrame;

static inline _spprof_InterpreterFrame*
_spprof_get_current_frame(PyThreadState *tstate) {
    if (tstate == NULL) return NULL;
    /* 3.13 has current_frame directly in tstate (no cframe) */
    return (_spprof_InterpreterFrame *)tstate->current_frame;
}

static inline PyCodeObject*
_spprof_frame_get_code(_spprof_InterpreterFrame *frame) {
    if (frame == NULL) return NULL;
    PyObject *exec = frame->f_executable;
    if (exec == NULL) return NULL;
    /* f_executable could be non-code in some cases (e.g., None for shim frames) */
    if (PyCode_Check(exec)) {
        return (PyCodeObject *)exec;
    }
    return NULL;
}

#endif /* SPPROF_PY313 */

/*
 * =============================================================================
 * Python 3.14 Frame Structures
 * =============================================================================
 *
 * 3.14 changes from 3.13:
 *   - f_executable and f_funcobj are now _PyStackRef (tagged pointers)
 *   - Added stackpointer field
 *   - Added tlbc_index for free-threading (thread-local bytecode index)
 *   - Added visited field for GC
 *
 * FREE-THREADING CONSIDERATIONS (Py_GIL_DISABLED):
 *
 * In free-threaded Python 3.14, the GIL is removed and multiple threads
 * can execute Python bytecode simultaneously. This has major implications:
 *
 * 1. FRAME CHAIN STABILITY:
 *    - Without the GIL, a thread's frame chain can change at any time
 *    - Function calls/returns modify the 'previous' pointer
 *    - Reading frame->previous while the thread is running is UNSAFE
 *
 * 2. TAGGED POINTERS (_PyStackRef):
 *    - f_executable and f_funcobj use tagged pointers for deferred refcounting
 *    - The low bits contain tag information, not part of the pointer
 *    - Must mask out tag bits before dereferencing
 *
 * 3. THREAD-LOCAL BYTECODE (tlbc_index):
 *    - Each thread may have its own copy of bytecode for cache optimization
 *    - The tlbc_index field identifies which thread-local copy is in use
 *    - Not relevant for sampling, but affects struct layout
 *
 * SAFE SAMPLING APPROACHES:
 *
 * A. Thread Suspension (Darwin/Mach):
 *    - thread_suspend() fully stops the target thread
 *    - Frame chain is stable during suspension
 *    - This is the PREFERRED approach for free-threaded Python
 *
 * B. Signal Handling (Linux SIGPROF):
 *    - Signal interrupts the thread at arbitrary point
 *    - Frame chain may be in inconsistent state
 *    - NOT SAFE without additional synchronization
 *    - Would need per-thread locks or atomic snapshot mechanisms
 *
 * C. Future: PEP 669 style callbacks
 *    - Python's own profiling callbacks run at safe points
 *    - Could be used as fallback on free-threaded builds
 */
#if SPPROF_PY314

typedef struct _spprof_InterpreterFrame {
    _spprof_StackRef f_executable;   /* Deferred or strong reference (code object or None) */
    struct _spprof_InterpreterFrame *previous;
    _spprof_StackRef f_funcobj;      /* Deferred or strong reference */
    PyObject *f_globals;             /* Borrowed reference */
    PyObject *f_builtins;            /* Borrowed reference */
    PyObject *f_locals;              /* Strong reference, may be NULL */
    PyFrameObject *frame_obj;        /* Strong reference, may be NULL */
    _Py_CODEUNIT *instr_ptr;         /* Instruction currently executing */
    void *stackpointer;              /* Stack pointer (opaque) */
#ifdef Py_GIL_DISABLED
    int32_t tlbc_index;              /* Thread-local bytecode index for free-threading */
#endif
    uint16_t return_offset;
    char owner;
    uint8_t visited;                 /* For GC/debugging */
    /* Followed by localsplus array */
} _spprof_InterpreterFrame;

/*
 * Extract PyObject* from a _PyStackRef (tagged pointer).
 *
 * Uses SPPROF_STACKREF_TAG_MASK to clear tag bits from the pointer.
 * See the mask definition above for the tagging scheme details.
 *
 * THREAD SAFETY:
 *   - This function is async-signal-safe (just bit manipulation)
 *   - However, the POINTER ITSELF may become invalid in free-threaded builds
 *     if the target thread is not suspended
 *   - Always ensure target thread is stopped before calling
 *
 * VERSION NOTE:
 *   If Python changes the tagging scheme, update SPPROF_STACKREF_TAG_MASK.
 *   The current implementation is validated against CPython 3.14.
 */
static inline PyObject*
_spprof_stackref_get(const _spprof_StackRef *ref) {
    if (ref == NULL) return NULL;
    /* Clear tag bits using the version-specific mask */
    return (PyObject *)(ref->bits & ~SPPROF_STACKREF_TAG_MASK);
}

static inline _spprof_InterpreterFrame*
_spprof_get_current_frame(PyThreadState *tstate) {
    if (tstate == NULL) return NULL;
    /* 
     * 3.14 has current_frame directly in tstate.
     * 
     * FREE-THREADING WARNING:
     * In free-threaded builds, reading current_frame from another thread's
     * state is only safe if:
     *   1. The target thread is suspended (Mach sampler), OR
     *   2. The target thread is the current thread (signal handler)
     */
    return (_spprof_InterpreterFrame *)tstate->current_frame;
}

static inline PyCodeObject*
_spprof_frame_get_code(_spprof_InterpreterFrame *frame) {
    if (frame == NULL) return NULL;
    PyObject *exec = _spprof_stackref_get(&frame->f_executable);
    if (exec == NULL) return NULL;
    /*
     * f_executable could be non-code in some cases (e.g., None for shim frames).
     *
     * WARNING: PyCode_Check() is NOT async-signal-safe as it may need to
     * access type objects. In signal handlers, we rely on:
     *   1. The GIL protecting type object access, OR
     *   2. Thread suspension ensuring stability
     *
     * In free-threaded builds with signal handlers, this could race.
     */
    if (PyCode_Check(exec)) {
        return (PyCodeObject *)exec;
    }
    return NULL;
}

#endif /* SPPROF_PY314 */

/*
 * =============================================================================
 * Common Functions (all versions)
 * =============================================================================
 */

static inline _spprof_InterpreterFrame*
_spprof_frame_get_previous(_spprof_InterpreterFrame *frame) {
    return frame ? frame->previous : NULL;
}

static inline int
_spprof_frame_is_shim(_spprof_InterpreterFrame *frame) {
    return frame && frame->owner == SPPROF_FRAME_OWNED_BY_CSTACK;
}

static inline int
_spprof_frame_get_owner(_spprof_InterpreterFrame *frame) {
    return frame ? frame->owner : -1;
}

/*
 * Get instruction pointer for line number calculation.
 * Returns a raw pointer to the current instruction.
 * We use void* to avoid type definition issues across versions.
 */
static inline void*
_spprof_frame_get_instr_ptr(_spprof_InterpreterFrame *frame) {
    if (frame == NULL) return NULL;
#if SPPROF_PY311 || SPPROF_PY312
    return (void*)frame->prev_instr;
#else
    /* Python 3.13+ uses instr_ptr */
    return (void*)frame->instr_ptr;
#endif
}

#ifdef __cplusplus
}
#endif

#endif /* SPPROF_INTERNAL_PYCORE_FRAME_H */
