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
 *   - Python 3.9.x
 *   - Python 3.10.x
 *   - Python 3.11.x
 *   - Python 3.12.x
 *   - Python 3.13.x
 *   - Python 3.14.x
 *
 * References:
 *   - CPython Include/cpython/frameobject.h (3.9/3.10)
 *   - CPython Include/internal/pycore_frame.h (3.11+)
 *   - CPython Include/cpython/pystate.h
 *
 * ASYNC-SIGNAL-SAFETY:
 *   All inline accessor functions in this header are async-signal-safe:
 *   - No Python C API calls (except PyCode_Check in 3.13+ which is unavoidable)
 *   - No memory allocation
 *   - No locks
 *   - Direct struct field access only
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
 *
 * Version macros for compile-time dispatch. Only ONE of these will be true
 * at compile time, allowing version-specific code paths.
 */

#define SPPROF_PY39  (PY_VERSION_HEX >= 0x03090000 && PY_VERSION_HEX < 0x030A0000)
#define SPPROF_PY310 (PY_VERSION_HEX >= 0x030A0000 && PY_VERSION_HEX < 0x030B0000)
#define SPPROF_PY311 (PY_VERSION_HEX >= 0x030B0000 && PY_VERSION_HEX < 0x030C0000)
#define SPPROF_PY312 (PY_VERSION_HEX >= 0x030C0000 && PY_VERSION_HEX < 0x030D0000)
#define SPPROF_PY313 (PY_VERSION_HEX >= 0x030D0000 && PY_VERSION_HEX < 0x030E0000)
#define SPPROF_PY314 (PY_VERSION_HEX >= 0x030E0000)

/* Minimum supported version is Python 3.9 */
#if PY_VERSION_HEX < 0x03090000
    #error "spprof requires Python 3.9 or later"
#endif

/*
 * Include frameobject.h for Python 3.9 to get PyTryBlock definition.
 * This is safe to include on all versions but only needed for 3.9.
 */
#if SPPROF_PY39
#include <frameobject.h>
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
 *
 * ASYNC-SIGNAL-SAFE: These are just integer constants, safe to use anywhere.
 */
enum _spprof_frameowner {
    SPPROF_FRAME_OWNED_BY_THREAD = 0,
    SPPROF_FRAME_OWNED_BY_GENERATOR = 1,
    SPPROF_FRAME_OWNED_BY_FRAME_OBJECT = 2,
    SPPROF_FRAME_OWNED_BY_CSTACK = 3,
};

/*
 * =============================================================================
 * Python 3.9 Frame Structures
 * =============================================================================
 *
 * In Python 3.9, PyFrameObject is the frame representation and is directly
 * accessible. The frame chain is linked via f_back pointers.
 *
 * Thread state access: tstate->frame points to the current frame.
 *
 * Source: CPython Include/cpython/frameobject.h (tag: v3.9.0 - v3.9.21)
 *
 * ASYNC-SIGNAL-SAFETY:
 *   - All accessors below are async-signal-safe
 *   - No Python C API calls
 *   - Direct memory reads only
 */
#if SPPROF_PY39

/*
 * CO_MAXBLOCKS is defined in CPython's code.h but may not be available
 * in all build configurations. Define it if missing.
 */
#ifndef CO_MAXBLOCKS
#define CO_MAXBLOCKS 20
#endif

/*
 * Python 3.9 PyFrameObject layout - EXACT match to CPython 3.9.x
 *
 * Note: This struct is larger than what we need for frame walking,
 * but we define the full layout to ensure correct field offsets.
 */
typedef struct _spprof_PyFrameObject_39 {
    PyObject_VAR_HEAD
    struct _spprof_PyFrameObject_39 *f_back;  /* Previous frame (toward caller) */
    PyCodeObject *f_code;                      /* Code object */
    PyObject *f_builtins;                      /* Builtins namespace */
    PyObject *f_globals;                       /* Global namespace */
    PyObject *f_locals;                        /* Local namespace (may be NULL) */
    PyObject **f_valuestack;                   /* Points after local variables */
    /* Running state */
    PyObject **f_stacktop;                     /* Stack top pointer */
    PyObject *f_trace;                         /* Trace function (may be NULL) */
    char f_trace_lines;                        /* Emit per-line trace events */
    char f_trace_opcodes;                      /* Emit per-opcode trace events */
    PyObject *f_gen;                           /* Generator/coroutine if this is a gen frame */
    int f_lasti;                               /* Last instruction (byte offset) */
    int f_lineno;                              /* Current line number */
    int f_iblock;                              /* Current block stack depth */
    char f_executing;                          /* Is the frame currently executing? */
    PyTryBlock f_blockstack[CO_MAXBLOCKS];     /* Block stack */
    PyObject *f_localsplus[1];                 /* Local variables + cells + freevars */
} _spprof_PyFrameObject_39;

/* Use a typedef for the version-specific frame type */
typedef _spprof_PyFrameObject_39 _spprof_PyFrameObject;

/*
 * Accessors for Python 3.9
 *
 * ASYNC-SIGNAL-SAFE: All functions below perform only direct memory reads.
 */

/* Get current frame from thread state */
static inline _spprof_PyFrameObject*
_spprof_get_current_frame(PyThreadState *tstate) {
    if (tstate == NULL) return NULL;
    /* In Python 3.9, tstate->frame is the current PyFrameObject */
    return (_spprof_PyFrameObject *)tstate->frame;
}

/* Get previous frame in call chain */
static inline _spprof_PyFrameObject*
_spprof_frame_get_previous(_spprof_PyFrameObject *frame) {
    return frame ? frame->f_back : NULL;
}

/* Get code object from frame */
static inline PyCodeObject*
_spprof_frame_get_code(_spprof_PyFrameObject *frame) {
    return frame ? frame->f_code : NULL;
}

/*
 * Get instruction pointer for line number calculation.
 *
 * In Python 3.9, f_lasti is a byte offset into co_code.
 * We compute the actual pointer for consistency with 3.11+.
 *
 * Note: co_code is a bytes object in 3.9.
 */
static inline void*
_spprof_frame_get_instr_ptr(_spprof_PyFrameObject *frame) {
    if (frame == NULL || frame->f_code == NULL) return NULL;
    if (frame->f_lasti < 0) return NULL;
    PyObject *co_code = frame->f_code->co_code;
    if (co_code == NULL) return NULL;
    return (void*)(PyBytes_AS_STRING(co_code) + frame->f_lasti);
}

/*
 * Check if frame is a shim frame.
 *
 * Python 3.9 doesn't have explicit shim frames (those were added in 3.11
 * with the _PyInterpreterFrame restructuring). Always returns 0.
 */
static inline int
_spprof_frame_is_shim(_spprof_PyFrameObject *frame) {
    (void)frame;  /* Unused */
    return 0;
}

/*
 * Get frame ownership type.
 *
 * Python 3.9 doesn't have an explicit owner field. We infer ownership:
 * - If the code object has generator/coroutine flags, it's generator-owned
 * - Otherwise, it's thread-owned
 *
 * Note: We can't detect FRAME_OWNED_BY_FRAME_OBJECT or CSTACK in 3.9.
 */
static inline int
_spprof_frame_get_owner(_spprof_PyFrameObject *frame) {
    if (frame == NULL) return -1;
    /* Detect generator/coroutine via code flags */
    if (frame->f_code != NULL) {
        int flags = frame->f_code->co_flags;
        if (flags & (CO_GENERATOR | CO_COROUTINE | CO_ASYNC_GENERATOR)) {
            return SPPROF_FRAME_OWNED_BY_GENERATOR;
        }
    }
    return SPPROF_FRAME_OWNED_BY_THREAD;
}

#endif /* SPPROF_PY39 */

/*
 * =============================================================================
 * Python 3.10 Frame Structures
 * =============================================================================
 *
 * Python 3.10 still uses PyFrameObject but with some layout changes:
 * - f_stacktop removed, replaced with f_stackdepth
 * - f_gen removed, replaced with f_gen_or_coro flag
 * - f_iblock and f_blockstack removed
 * - f_executing removed
 * - f_state enum added (PyFrameState)
 *
 * Thread state access: tstate->frame points to the current frame.
 *
 * Source: CPython Include/cpython/frameobject.h (tag: v3.10.0 - v3.10.16)
 *
 * ASYNC-SIGNAL-SAFETY:
 *   - All accessors below are async-signal-safe
 *   - No Python C API calls
 *   - Direct memory reads only
 */
#if SPPROF_PY310

/*
 * PyFrameState enum values for Python 3.10
 *
 * We define our own enum to avoid depending on internal headers.
 */
typedef enum _spprof_PyFrameState {
    SPPROF_FRAME_CREATED = -2,
    SPPROF_FRAME_SUSPENDED = -1,
    SPPROF_FRAME_EXECUTING = 0,
    SPPROF_FRAME_COMPLETED = 1,
    SPPROF_FRAME_CLEARED = 4,
} _spprof_PyFrameState;

/*
 * Python 3.10 PyFrameObject layout - EXACT match to CPython 3.10.x
 */
typedef struct _spprof_PyFrameObject_310 {
    PyObject_VAR_HEAD
    struct _spprof_PyFrameObject_310 *f_back;  /* Previous frame (toward caller) */
    PyCodeObject *f_code;                       /* Code object */
    PyObject *f_builtins;                       /* Builtins namespace */
    PyObject *f_globals;                        /* Global namespace */
    PyObject *f_locals;                         /* Local namespace (may be NULL) */
    PyObject **f_valuestack;                    /* Points after local variables */
    PyObject *f_trace;                          /* Trace function (may be NULL) */
    int f_stackdepth;                           /* Depth of value stack */
    char f_trace_lines;                         /* Emit per-line trace events */
    char f_trace_opcodes;                       /* Emit per-opcode trace events */
    char f_gen_or_coro;                         /* True if generator/coroutine frame */
    /* State and execution info */
    PyFrameState f_state;                       /* Frame state enum */
    int f_lasti;                                /* Last instruction (byte offset) */
    int f_lineno;                               /* Current line number (when tracing) */
    /* Note: f_localsplus follows but is variable-sized */
} _spprof_PyFrameObject_310;

/* Use a typedef for the version-specific frame type */
typedef _spprof_PyFrameObject_310 _spprof_PyFrameObject;

/*
 * Accessors for Python 3.10
 *
 * ASYNC-SIGNAL-SAFE: All functions below perform only direct memory reads.
 */

/* Get current frame from thread state */
static inline _spprof_PyFrameObject*
_spprof_get_current_frame(PyThreadState *tstate) {
    if (tstate == NULL) return NULL;
    /* In Python 3.10, tstate->frame is the current PyFrameObject */
    return (_spprof_PyFrameObject *)tstate->frame;
}

/* Get previous frame in call chain */
static inline _spprof_PyFrameObject*
_spprof_frame_get_previous(_spprof_PyFrameObject *frame) {
    return frame ? frame->f_back : NULL;
}

/* Get code object from frame */
static inline PyCodeObject*
_spprof_frame_get_code(_spprof_PyFrameObject *frame) {
    return frame ? frame->f_code : NULL;
}

/*
 * Get instruction pointer for line number calculation.
 *
 * In Python 3.10, f_lasti is still a byte offset into co_code.
 * Note: co_code is a bytes object in 3.10.
 */
static inline void*
_spprof_frame_get_instr_ptr(_spprof_PyFrameObject *frame) {
    if (frame == NULL || frame->f_code == NULL) return NULL;
    if (frame->f_lasti < 0) return NULL;
    PyObject *co_code = frame->f_code->co_code;
    if (co_code == NULL) return NULL;
    return (void*)(PyBytes_AS_STRING(co_code) + frame->f_lasti);
}

/*
 * Check if frame is a shim frame.
 *
 * Python 3.10 doesn't have explicit shim frames. Always returns 0.
 */
static inline int
_spprof_frame_is_shim(_spprof_PyFrameObject *frame) {
    (void)frame;  /* Unused */
    return 0;
}

/*
 * Get frame ownership type.
 *
 * Python 3.10 has the f_gen_or_coro field which makes this easier
 * than 3.9's code flag check.
 */
static inline int
_spprof_frame_get_owner(_spprof_PyFrameObject *frame) {
    if (frame == NULL) return -1;
    /* Use f_gen_or_coro field for ownership detection */
    if (frame->f_gen_or_coro) {
        return SPPROF_FRAME_OWNED_BY_GENERATOR;
    }
    return SPPROF_FRAME_OWNED_BY_THREAD;
}

#endif /* SPPROF_PY310 */

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
 * Common Functions for Python 3.11+ (InterpreterFrame-based)
 * =============================================================================
 *
 * These functions are only defined for Python 3.11+ where we use
 * _spprof_InterpreterFrame. Python 3.9/3.10 use _spprof_PyFrameObject
 * and have their own accessors defined above.
 *
 * ASYNC-SIGNAL-SAFETY:
 *   All functions below are async-signal-safe:
 *   - Direct struct field access only
 *   - No Python C API calls
 *   - No memory allocation
 */
#if SPPROF_PY311 || SPPROF_PY312 || SPPROF_PY313 || SPPROF_PY314

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
 *
 * ASYNC-SIGNAL-SAFE: Direct memory read.
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

#endif /* SPPROF_PY311 || SPPROF_PY312 || SPPROF_PY313 || SPPROF_PY314 */

#ifdef __cplusplus
}
#endif

#endif /* SPPROF_INTERNAL_PYCORE_FRAME_H */
