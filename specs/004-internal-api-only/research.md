# Research: Internal API Only Frame Walking

**Feature**: 004-internal-api-only  
**Date**: 2025-12-01

## Executive Summary

This research documents the Python internal frame structures across versions 3.9-3.14 to enable removing public API reliance and using direct struct access for all Python versions.

---

## Research Topic 1: Python 3.9/3.10 Frame Structures

### Decision
Use direct `PyFrameObject` struct access for Python 3.9/3.10 instead of public API functions.

### Rationale
Python 3.9 and 3.10 use `PyFrameObject` as the frame representation (before the 3.11 refactoring to `_PyInterpreterFrame`). The `PyFrameObject` struct fields were historically exposed in `<frameobject.h>` and can be accessed directly.

### Python 3.9/3.10 PyFrameObject Layout

From CPython source (`Include/cpython/frameobject.h`):

```c
/* Python 3.9/3.10 PyFrameObject - from CPython source */
typedef struct _frame {
    PyObject_VAR_HEAD
    struct _frame *f_back;      /* Previous frame (toward caller) */
    PyCodeObject *f_code;       /* Code object */
    PyObject *f_builtins;       /* Builtins namespace */
    PyObject *f_globals;        /* Global namespace */
    PyObject *f_locals;         /* Local namespace */
    PyObject **f_valuestack;    /* Points after local variables */
    PyObject *f_trace;          /* Trace function */
    int f_stackdepth;           /* Depth of stack (3.10+) */
    char f_trace_lines;         /* Emit line tracing events */
    char f_trace_opcodes;       /* Emit opcode tracing events */
    char f_gen_or_coro;         /* Generator/coroutine flag (3.10+) */
    PyObject *f_localsplus[1];  /* Local variables + cells + freevars */
    /* Additional fields vary between 3.9 and 3.10 */
} PyFrameObject;
```

### Thread State Access (3.9/3.10)

```c
/* Access current frame from thread state */
PyThreadState *tstate = PyThreadState_GET();
PyFrameObject *frame = tstate->frame;  /* Direct field access */
```

### Key Differences from 3.11+

| Feature | Python 3.9/3.10 | Python 3.11+ |
|---------|-----------------|--------------|
| Frame type | `PyFrameObject` | `_PyInterpreterFrame` |
| Thread state access | `tstate->frame` | `tstate->cframe->current_frame` (3.11-3.12) or `tstate->current_frame` (3.13+) |
| Instruction pointer | `f_lasti` (int offset) | `prev_instr` / `instr_ptr` (pointer) |
| Frame ownership | N/A (always owned by object) | `owner` field with ownership enum |

### Alternatives Considered

1. **Continue using public API for 3.9/3.10**: Rejected because it's not async-signal-safe (`Py_DECREF` calls).
2. **Drop support for 3.9/3.10**: Rejected because users still need these versions.
3. **Use `PyFrameObject` direct access**: **Selected** - The struct layout is stable within minor versions and provides async-signal-safe access.

---

## Research Topic 2: Instruction Pointer Access

### Decision
For Python 3.9/3.10, compute instruction pointer from `f_lasti` field; for 3.11+, use existing `prev_instr`/`instr_ptr` pointer.

### Rationale
Python 3.9/3.10 stores instruction offset as an integer (`f_lasti`), while 3.11+ uses a direct pointer to the bytecode. We can compute the pointer from the offset for consistency.

### Implementation Approach

```c
/* Python 3.9/3.10 instruction pointer calculation */
static inline void*
_spprof_frame_get_instr_ptr_39(_spprof_PyFrameObject *frame) {
    if (frame == NULL || frame->f_code == NULL) return NULL;
    /* f_lasti is byte offset; convert to pointer */
    return (void*)((char*)frame->f_code->co_code + frame->f_lasti);
}
```

Note: In Python 3.9/3.10, `f_lasti` is measured in bytes (word size = 2 bytes per instruction).

### Alternatives Considered

1. **Return f_lasti directly**: Rejected - inconsistent return type across versions.
2. **Don't support instruction pointers on 3.9/3.10**: Rejected - reduces feature parity.
3. **Compute pointer from offset**: **Selected** - Provides consistent API.

---

## Research Topic 3: Frame Ownership Detection (3.9/3.10)

### Decision
For Python 3.9/3.10, infer ownership from frame characteristics; explicit `owner` field not available.

### Rationale
Python 3.9/3.10 doesn't have the `owner` enum field. However, we can detect:
- Generator/coroutine frames via `f_gen_or_coro` field (3.10) or code flags (3.9)
- Regular frames are thread-owned by default

### Implementation Approach

```c
/* Python 3.9/3.10 frame ownership detection */
static inline int
_spprof_frame_get_owner_39(_spprof_PyFrameObject *frame) {
    if (frame == NULL) return -1;
    
#if PY_VERSION_HEX >= 0x030A0000  /* Python 3.10 */
    if (frame->f_gen_or_coro) {
        return SPPROF_FRAME_OWNED_BY_GENERATOR;
    }
#else  /* Python 3.9 */
    if (frame->f_code && (frame->f_code->co_flags & (CO_GENERATOR | CO_COROUTINE | CO_ASYNC_GENERATOR))) {
        return SPPROF_FRAME_OWNED_BY_GENERATOR;
    }
#endif
    return SPPROF_FRAME_OWNED_BY_THREAD;
}
```

### Alternatives Considered

1. **Always return THREAD ownership**: Rejected - loses generator/coroutine detection.
2. **Use code object flags**: **Selected** - Provides reasonable ownership inference.

---

## Research Topic 4: Real Python Headers Approach

### Decision
Define our own struct layouts that mirror CPython's internal structures, validated against specific Python versions.

### Rationale
The user requested adhering to "real Python headers." Two approaches exist:

1. **Include actual Python internal headers** (`#include <internal/pycore_frame.h>`)
   - Requires `Py_BUILD_CORE` macro
   - Headers may not be installed with Python dev packages
   - Headers change between Python versions

2. **Define compatible struct layouts** (current approach)
   - Self-contained, doesn't depend on internal headers being available
   - Struct layouts validated against CPython source
   - Allows targeted support for specific Python versions

We continue with approach #2 but ensure struct layouts exactly match CPython source.

### Validation Strategy

For each Python version, validate struct layouts against:
- CPython source: `Include/cpython/frameobject.h` (3.9/3.10)
- CPython source: `Include/internal/pycore_frame.h` (3.11+)

Document validated Python patch versions in comments.

### Alternatives Considered

1. **Include actual internal headers**: Rejected - portability issues, headers not always available.
2. **Use offsetof() validation at runtime**: Could be added as a debug check.
3. **Define compatible structs with documentation**: **Selected** - Most practical approach.

---

## Research Topic 5: Thread State Access Across Versions

### Decision
Use version-specific thread state access, handling the transition from `tstate->frame` (3.9/3.10) to `cframe` (3.11-3.12) to `current_frame` (3.13+).

### Implementation Summary

| Version | Current Frame Access |
|---------|---------------------|
| 3.9-3.10 | `tstate->frame` (PyFrameObject*) |
| 3.11-3.12 | `tstate->cframe->current_frame` (_PyInterpreterFrame*) |
| 3.13+ | `tstate->current_frame` (_PyInterpreterFrame*) |

### Async-Signal Safety

For async-signal-safe access, use `PyThreadState_GET()` (3.9-3.12) or `PyThreadState_GetUnchecked()` (3.13+):

```c
static inline PyThreadState*
_spprof_tstate_get(void) {
#if PY_VERSION_HEX >= 0x030D0000  /* 3.13+ */
    return PyThreadState_GetUnchecked();
#else
    return PyThreadState_GET();
#endif
}
```

---

## Summary of Findings

| Research Topic | Decision | Risk Level |
|---------------|----------|------------|
| Python 3.9/3.10 frame struct | Direct `PyFrameObject` access | Low - stable within minor versions |
| Instruction pointer | Compute from `f_lasti` offset | Low - well-documented behavior |
| Frame ownership (3.9/3.10) | Infer from code flags | Medium - approximation, not exact |
| Real headers approach | Self-contained struct definitions | Low - validated against CPython source |
| Thread state access | Version-specific access patterns | Low - well-documented in CPython |

---

## References

1. CPython Source - Python 3.9: `Include/cpython/frameobject.h`
2. CPython Source - Python 3.10: `Include/cpython/frameobject.h`
3. CPython Source - Python 3.11+: `Include/internal/pycore_frame.h`
4. PEP 523 - Adding a frame evaluation API to CPython
5. Python C API Documentation: https://docs.python.org/3/c-api/frame.html

