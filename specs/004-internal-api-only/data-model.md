# Data Model: Internal API Frame Structures

**Feature**: 004-internal-api-only  
**Date**: 2025-12-01

## Overview

This document defines the internal frame structures for all supported Python versions (3.9-3.14). These structures mirror CPython's internal layouts and enable async-signal-safe frame walking without relying on public API functions.

---

## Version Detection Macros

```c
/* Version detection - must be defined before including any Python headers */
#define SPPROF_PY39  (PY_VERSION_HEX >= 0x03090000 && PY_VERSION_HEX < 0x030A0000)
#define SPPROF_PY310 (PY_VERSION_HEX >= 0x030A0000 && PY_VERSION_HEX < 0x030B0000)
#define SPPROF_PY311 (PY_VERSION_HEX >= 0x030B0000 && PY_VERSION_HEX < 0x030C0000)
#define SPPROF_PY312 (PY_VERSION_HEX >= 0x030C0000 && PY_VERSION_HEX < 0x030D0000)
#define SPPROF_PY313 (PY_VERSION_HEX >= 0x030D0000 && PY_VERSION_HEX < 0x030E0000)
#define SPPROF_PY314 (PY_VERSION_HEX >= 0x030E0000)
```

---

## Entity: Frame Ownership Constants

**Purpose**: Enum defining who owns a frame (consistent across all versions).

```c
enum _spprof_frameowner {
    SPPROF_FRAME_OWNED_BY_THREAD = 0,      /* Regular execution */
    SPPROF_FRAME_OWNED_BY_GENERATOR = 1,   /* Generator/coroutine */
    SPPROF_FRAME_OWNED_BY_FRAME_OBJECT = 2, /* User-visible frame */
    SPPROF_FRAME_OWNED_BY_CSTACK = 3,      /* C-level shim frame */
};
```

---

## Entity: Python 3.9 PyFrameObject

**Source**: CPython `Include/cpython/frameobject.h` (tag: v3.9.0)  
**Validated Against**: Python 3.9.0 - 3.9.21

```c
#if SPPROF_PY39

typedef struct _spprof_PyFrameObject {
    PyObject_VAR_HEAD
    struct _spprof_PyFrameObject *f_back;  /* Previous frame (toward caller) */
    PyCodeObject *f_code;                   /* Code object */
    PyObject *f_builtins;                   /* Builtins namespace */
    PyObject *f_globals;                    /* Global namespace */
    PyObject *f_locals;                     /* Local namespace (may be NULL) */
    PyObject **f_valuestack;                /* Points after local variables */
    /* Running state */
    PyObject **f_stacktop;                  /* Stack top pointer */
    PyObject *f_trace;                      /* Trace function (may be NULL) */
    char f_trace_lines;                     /* Emit per-line trace events */
    char f_trace_opcodes;                   /* Emit per-opcode trace events */
    PyObject *f_gen;                        /* Generator/coroutine if this is a gen frame */
    int f_lasti;                            /* Last instruction (byte offset) */
    int f_lineno;                           /* Current line number */
    int f_iblock;                           /* Current block stack depth */
    char f_executing;                       /* Is the frame currently executing? */
    PyTryBlock f_blockstack[CO_MAXBLOCKS];  /* Block stack */
    PyObject *f_localsplus[1];              /* Local variables + cells + freevars */
} _spprof_PyFrameObject;

/* Accessors for Python 3.9 */
static inline _spprof_PyFrameObject*
_spprof_get_current_frame(PyThreadState *tstate) {
    if (tstate == NULL) return NULL;
    return (_spprof_PyFrameObject *)tstate->frame;
}

static inline _spprof_PyFrameObject*
_spprof_frame_get_previous(_spprof_PyFrameObject *frame) {
    return frame ? frame->f_back : NULL;
}

static inline PyCodeObject*
_spprof_frame_get_code(_spprof_PyFrameObject *frame) {
    return frame ? frame->f_code : NULL;
}

static inline void*
_spprof_frame_get_instr_ptr(_spprof_PyFrameObject *frame) {
    if (frame == NULL || frame->f_code == NULL) return NULL;
    if (frame->f_lasti < 0) return NULL;
    /* f_lasti is byte offset into co_code */
    PyObject *co_code = frame->f_code->co_code;
    if (co_code == NULL) return NULL;
    return (void*)(PyBytes_AS_STRING(co_code) + frame->f_lasti);
}

static inline int
_spprof_frame_is_shim(_spprof_PyFrameObject *frame) {
    /* Python 3.9 doesn't have explicit shim frames */
    return 0;
}

static inline int
_spprof_frame_get_owner(_spprof_PyFrameObject *frame) {
    if (frame == NULL) return -1;
    /* Detect generator/coroutine via code flags */
    if (frame->f_code && (frame->f_code->co_flags & 
        (CO_GENERATOR | CO_COROUTINE | CO_ASYNC_GENERATOR))) {
        return SPPROF_FRAME_OWNED_BY_GENERATOR;
    }
    return SPPROF_FRAME_OWNED_BY_THREAD;
}

#endif /* SPPROF_PY39 */
```

---

## Entity: Python 3.10 PyFrameObject

**Source**: CPython `Include/cpython/frameobject.h` (tag: v3.10.0)  
**Validated Against**: Python 3.10.0 - 3.10.16

```c
#if SPPROF_PY310

typedef struct _spprof_PyFrameObject {
    PyObject_VAR_HEAD
    struct _spprof_PyFrameObject *f_back;  /* Previous frame (toward caller) */
    PyCodeObject *f_code;                   /* Code object */
    PyObject *f_builtins;                   /* Builtins namespace */
    PyObject *f_globals;                    /* Global namespace */
    PyObject *f_locals;                     /* Local namespace (may be NULL) */
    PyObject **f_valuestack;                /* Points after local variables */
    PyObject *f_trace;                      /* Trace function (may be NULL) */
    int f_stackdepth;                       /* Depth of value stack */
    char f_trace_lines;                     /* Emit per-line trace events */
    char f_trace_opcodes;                   /* Emit per-opcode trace events */
    char f_gen_or_coro;                     /* True if generator/coroutine frame */
    /* State and exception info */
    PyFrameState f_state;                   /* Frame state enum */
    int f_lasti;                            /* Last instruction (byte offset) */
    int f_lineno;                           /* Current line number (when tracing) */
    /* Note: f_localsplus follows but is variable-sized */
} _spprof_PyFrameObject;

/* Note: PyFrameState enum values (from Python 3.10) */
typedef enum _spprof_framestate {
    SPPROF_FRAME_CREATED = -2,
    SPPROF_FRAME_SUSPENDED = -1,
    SPPROF_FRAME_EXECUTING = 0,
    SPPROF_FRAME_COMPLETED = 1,
    SPPROF_FRAME_CLEARED = 4,
} _spprof_PyFrameState;

/* Accessors for Python 3.10 */
static inline _spprof_PyFrameObject*
_spprof_get_current_frame(PyThreadState *tstate) {
    if (tstate == NULL) return NULL;
    return (_spprof_PyFrameObject *)tstate->frame;
}

static inline _spprof_PyFrameObject*
_spprof_frame_get_previous(_spprof_PyFrameObject *frame) {
    return frame ? frame->f_back : NULL;
}

static inline PyCodeObject*
_spprof_frame_get_code(_spprof_PyFrameObject *frame) {
    return frame ? frame->f_code : NULL;
}

static inline void*
_spprof_frame_get_instr_ptr(_spprof_PyFrameObject *frame) {
    if (frame == NULL || frame->f_code == NULL) return NULL;
    if (frame->f_lasti < 0) return NULL;
    /* f_lasti is byte offset; in 3.10 word size is 2 bytes */
    PyObject *co_code = frame->f_code->co_code;
    if (co_code == NULL) return NULL;
    return (void*)(PyBytes_AS_STRING(co_code) + frame->f_lasti);
}

static inline int
_spprof_frame_is_shim(_spprof_PyFrameObject *frame) {
    /* Python 3.10 doesn't have explicit shim frames */
    return 0;
}

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
```

---

## Entity: Python 3.11 _PyInterpreterFrame

**Source**: CPython `Include/internal/pycore_frame.h` (tag: v3.11.0)  
**Validated Against**: Python 3.11.0 - 3.11.11

```c
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

/* CFrame structure for thread state access */
typedef struct _spprof_CFrame {
    uint8_t use_tracing;         /* 0 or 255 (offset 0) */
    /* 7 bytes padding for alignment */
    _spprof_InterpreterFrame *current_frame;  /* (offset 8) */
    struct _spprof_CFrame *previous;          /* (offset 16) */
} _spprof_CFrame;

/* Accessors for Python 3.11 */
static inline _spprof_InterpreterFrame*
_spprof_get_current_frame(PyThreadState *tstate) {
    if (tstate == NULL) return NULL;
    _spprof_CFrame *cf = (_spprof_CFrame *)tstate->cframe;
    return cf ? cf->current_frame : NULL;
}

static inline _spprof_InterpreterFrame*
_spprof_frame_get_previous(_spprof_InterpreterFrame *frame) {
    return frame ? frame->previous : NULL;
}

static inline PyCodeObject*
_spprof_frame_get_code(_spprof_InterpreterFrame *frame) {
    return frame ? frame->f_code : NULL;
}

static inline void*
_spprof_frame_get_instr_ptr(_spprof_InterpreterFrame *frame) {
    return frame ? (void*)frame->prev_instr : NULL;
}

static inline int
_spprof_frame_is_shim(_spprof_InterpreterFrame *frame) {
    return frame && frame->owner == SPPROF_FRAME_OWNED_BY_CSTACK;
}

static inline int
_spprof_frame_get_owner(_spprof_InterpreterFrame *frame) {
    return frame ? frame->owner : -1;
}

#endif /* SPPROF_PY311 */
```

---

## Entity: Python 3.12 _PyInterpreterFrame

**Source**: CPython `Include/internal/pycore_frame.h` (tag: v3.12.0)  
**Validated Against**: Python 3.12.0 - 3.12.8

```c
#if SPPROF_PY312

typedef struct _spprof_InterpreterFrame {
    PyCodeObject *f_code;        /* Strong reference - first field! */
    struct _spprof_InterpreterFrame *previous;
    PyObject *f_funcobj;
    PyObject *f_globals;
    PyObject *f_builtins;
    PyObject *f_locals;
    PyFrameObject *frame_obj;
    _Py_CODEUNIT *prev_instr;    /* Named prev_instr in 3.12 */
    int stacktop;
    uint16_t return_offset;
    char owner;
    /* Followed by localsplus array */
} _spprof_InterpreterFrame;

/* CFrame structure - still exists in 3.12 */
typedef struct _spprof_CFrame {
    struct _spprof_InterpreterFrame *current_frame;
    struct _spprof_CFrame *previous;
} _spprof_CFrame;

/* Accessors for Python 3.12 */
static inline _spprof_InterpreterFrame*
_spprof_get_current_frame(PyThreadState *tstate) {
    if (tstate == NULL) return NULL;
    _spprof_CFrame *cf = (_spprof_CFrame *)tstate->cframe;
    return cf ? cf->current_frame : NULL;
}

static inline _spprof_InterpreterFrame*
_spprof_frame_get_previous(_spprof_InterpreterFrame *frame) {
    return frame ? frame->previous : NULL;
}

static inline PyCodeObject*
_spprof_frame_get_code(_spprof_InterpreterFrame *frame) {
    return frame ? frame->f_code : NULL;
}

static inline void*
_spprof_frame_get_instr_ptr(_spprof_InterpreterFrame *frame) {
    return frame ? (void*)frame->prev_instr : NULL;
}

static inline int
_spprof_frame_is_shim(_spprof_InterpreterFrame *frame) {
    return frame && frame->owner == SPPROF_FRAME_OWNED_BY_CSTACK;
}

static inline int
_spprof_frame_get_owner(_spprof_InterpreterFrame *frame) {
    return frame ? frame->owner : -1;
}

#endif /* SPPROF_PY312 */
```

---

## Entity: Python 3.13 _PyInterpreterFrame

**Source**: CPython `Include/internal/pycore_frame.h` (tag: v3.13.0)  
**Validated Against**: Python 3.13.0 - 3.13.1

```c
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

/* No CFrame in 3.13 - current_frame directly in tstate */

/* Accessors for Python 3.13 */
static inline _spprof_InterpreterFrame*
_spprof_get_current_frame(PyThreadState *tstate) {
    if (tstate == NULL) return NULL;
    return (_spprof_InterpreterFrame *)tstate->current_frame;
}

static inline _spprof_InterpreterFrame*
_spprof_frame_get_previous(_spprof_InterpreterFrame *frame) {
    return frame ? frame->previous : NULL;
}

static inline PyCodeObject*
_spprof_frame_get_code(_spprof_InterpreterFrame *frame) {
    if (frame == NULL) return NULL;
    PyObject *exec = frame->f_executable;
    if (exec == NULL) return NULL;
    if (PyCode_Check(exec)) {
        return (PyCodeObject *)exec;
    }
    return NULL;
}

static inline void*
_spprof_frame_get_instr_ptr(_spprof_InterpreterFrame *frame) {
    return frame ? (void*)frame->instr_ptr : NULL;
}

static inline int
_spprof_frame_is_shim(_spprof_InterpreterFrame *frame) {
    return frame && frame->owner == SPPROF_FRAME_OWNED_BY_CSTACK;
}

static inline int
_spprof_frame_get_owner(_spprof_InterpreterFrame *frame) {
    return frame ? frame->owner : -1;
}

#endif /* SPPROF_PY313 */
```

---

## Entity: Python 3.14 _PyInterpreterFrame

**Source**: CPython `Include/internal/pycore_frame.h` (tag: v3.14.0a2)  
**Validated Against**: Python 3.14.0a1 - 3.14.0a2

```c
#if SPPROF_PY314

/* Tagged pointer for 3.14 (deferred refcounting) */
typedef union _spprof_StackRef {
    uintptr_t bits;
} _spprof_StackRef;

#define SPPROF_STACKREF_TAG_MASK ((uintptr_t)0x3)

static inline PyObject*
_spprof_stackref_get(const _spprof_StackRef *ref) {
    if (ref == NULL) return NULL;
    return (PyObject *)(ref->bits & ~SPPROF_STACKREF_TAG_MASK);
}

typedef struct _spprof_InterpreterFrame {
    _spprof_StackRef f_executable;   /* Deferred or strong reference */
    struct _spprof_InterpreterFrame *previous;
    _spprof_StackRef f_funcobj;      /* Deferred or strong reference */
    PyObject *f_globals;             /* Borrowed reference */
    PyObject *f_builtins;            /* Borrowed reference */
    PyObject *f_locals;              /* Strong reference, may be NULL */
    PyFrameObject *frame_obj;        /* Strong reference, may be NULL */
    _Py_CODEUNIT *instr_ptr;         /* Instruction currently executing */
    void *stackpointer;              /* Stack pointer (opaque) */
#ifdef Py_GIL_DISABLED
    int32_t tlbc_index;              /* Thread-local bytecode index */
#endif
    uint16_t return_offset;
    char owner;
    uint8_t visited;                 /* For GC/debugging */
    /* Followed by localsplus array */
} _spprof_InterpreterFrame;

/* Accessors for Python 3.14 */
static inline _spprof_InterpreterFrame*
_spprof_get_current_frame(PyThreadState *tstate) {
    if (tstate == NULL) return NULL;
    return (_spprof_InterpreterFrame *)tstate->current_frame;
}

static inline _spprof_InterpreterFrame*
_spprof_frame_get_previous(_spprof_InterpreterFrame *frame) {
    return frame ? frame->previous : NULL;
}

static inline PyCodeObject*
_spprof_frame_get_code(_spprof_InterpreterFrame *frame) {
    if (frame == NULL) return NULL;
    PyObject *exec = _spprof_stackref_get(&frame->f_executable);
    if (exec == NULL) return NULL;
    if (PyCode_Check(exec)) {
        return (PyCodeObject *)exec;
    }
    return NULL;
}

static inline void*
_spprof_frame_get_instr_ptr(_spprof_InterpreterFrame *frame) {
    return frame ? (void*)frame->instr_ptr : NULL;
}

static inline int
_spprof_frame_is_shim(_spprof_InterpreterFrame *frame) {
    return frame && frame->owner == SPPROF_FRAME_OWNED_BY_CSTACK;
}

static inline int
_spprof_frame_get_owner(_spprof_InterpreterFrame *frame) {
    return frame ? frame->owner : -1;
}

#endif /* SPPROF_PY314 */
```

---

## Entity Relationships

```
PyThreadState
    │
    ├── [3.9/3.10] frame ──────────> _spprof_PyFrameObject
    │                                    │
    │                                    ├── f_back ──> previous frame
    │                                    ├── f_code ──> PyCodeObject
    │                                    └── f_lasti ── instruction offset
    │
    ├── [3.11/3.12] cframe ──────> _spprof_CFrame
    │                                    │
    │                                    └── current_frame ──> _spprof_InterpreterFrame
    │                                                              │
    │                                                              ├── previous ──> previous frame
    │                                                              ├── f_code ──> PyCodeObject
    │                                                              └── prev_instr ── instruction ptr
    │
    └── [3.13+] current_frame ───> _spprof_InterpreterFrame
                                        │
                                        ├── previous ──> previous frame
                                        ├── f_executable ──> PyCodeObject (or tagged ref in 3.14)
                                        └── instr_ptr ── instruction ptr
```

---

## Validation Rules

1. **Struct Size Validation**: At compile time or module init, verify struct sizes match expected values using `_Static_assert` or runtime checks.

2. **Field Offset Validation**: Use `offsetof()` to verify critical field positions match CPython layout.

3. **Version Compatibility**: Each struct is only compiled for its specific Python version range.

4. **Pointer Validation**: All pointer accesses must use `_spprof_ptr_valid()` before dereferencing.

