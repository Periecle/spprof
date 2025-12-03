# Contract: Internal Frame Walking API

**Feature**: 004-internal-api-only  
**Date**: 2025-12-01  
**Type**: C Internal API

## Overview

This contract defines the unified internal frame walking API that replaces the dual internal/public API approach. All functions are designed to be async-signal-safe where documented.

---

## Header Organization

### New Header: `internal/pycore_frame.h`

Unified header containing:
- Version detection macros (`SPPROF_PY39` through `SPPROF_PY314`)
- Frame struct definitions for all Python versions
- Accessor inline functions
- Common utilities (pointer validation, ownership constants)

### Removed Headers

The following headers will be removed:
- `compat/py39.h`
- `compat/py311.h`
- `compat/py312.h`
- `compat/py313.h`
- `compat/py314.h`

---

## API Functions

### Thread State Access

```c
/**
 * Get current thread state - ASYNC-SIGNAL-SAFE
 *
 * @return PyThreadState* or NULL if not in a Python thread
 */
static inline PyThreadState* _spprof_tstate_get(void);
```

**Behavior by Version**:
| Version | Implementation |
|---------|---------------|
| 3.9-3.12 | `PyThreadState_GET()` |
| 3.13+ | `PyThreadState_GetUnchecked()` |

---

### Frame Access

```c
/**
 * Get current frame from thread state - ASYNC-SIGNAL-SAFE
 *
 * @param tstate Thread state (may be NULL)
 * @return Frame pointer or NULL
 *
 * Return type varies by version:
 *   - 3.9/3.10: _spprof_PyFrameObject*
 *   - 3.11+: _spprof_InterpreterFrame*
 */
static inline void* _spprof_get_current_frame(PyThreadState *tstate);
```

**Behavior by Version**:
| Version | Access Pattern |
|---------|---------------|
| 3.9-3.10 | `tstate->frame` |
| 3.11-3.12 | `tstate->cframe->current_frame` |
| 3.13+ | `tstate->current_frame` |

---

### Frame Navigation

```c
/**
 * Get previous frame in call chain - ASYNC-SIGNAL-SAFE
 *
 * @param frame Current frame (may be NULL)
 * @return Previous frame or NULL if at stack bottom
 */
static inline void* _spprof_frame_get_previous(void *frame);
```

**Behavior by Version**:
| Version | Field |
|---------|-------|
| 3.9-3.10 | `frame->f_back` |
| 3.11+ | `frame->previous` |

---

### Code Object Access

```c
/**
 * Get code object from frame - ASYNC-SIGNAL-SAFE (with caveats on 3.13+)
 *
 * @param frame Frame pointer (may be NULL)
 * @return PyCodeObject* or NULL
 *
 * Note: On 3.13+, PyCode_Check() is called which may not be fully
 * async-signal-safe. For guaranteed safety, use thread suspension.
 */
static inline PyCodeObject* _spprof_frame_get_code(void *frame);
```

**Behavior by Version**:
| Version | Implementation |
|---------|---------------|
| 3.9-3.10 | `frame->f_code` |
| 3.11-3.12 | `frame->f_code` |
| 3.13 | `(PyCodeObject*)frame->f_executable` with type check |
| 3.14 | Untag `frame->f_executable` + type check |

---

### Instruction Pointer Access

```c
/**
 * Get instruction pointer from frame - ASYNC-SIGNAL-SAFE
 *
 * @param frame Frame pointer (may be NULL)
 * @return Instruction pointer or NULL
 *
 * For 3.9/3.10: Computes pointer from f_lasti offset
 * For 3.11+: Returns prev_instr or instr_ptr directly
 */
static inline void* _spprof_frame_get_instr_ptr(void *frame);
```

**Behavior by Version**:
| Version | Implementation |
|---------|---------------|
| 3.9-3.10 | `co_code + f_lasti` |
| 3.11-3.12 | `frame->prev_instr` |
| 3.13+ | `frame->instr_ptr` |

---

### Frame Ownership

```c
/**
 * Check if frame is a C-stack shim - ASYNC-SIGNAL-SAFE
 *
 * @param frame Frame pointer (may be NULL)
 * @return 1 if shim frame, 0 otherwise
 *
 * Note: Python 3.9/3.10 don't have shim frames, always returns 0.
 */
static inline int _spprof_frame_is_shim(void *frame);

/**
 * Get frame ownership type - ASYNC-SIGNAL-SAFE
 *
 * @param frame Frame pointer (may be NULL)
 * @return SPPROF_FRAME_OWNED_BY_* constant or -1 if NULL
 *
 * Note: Python 3.9/3.10 infer ownership from code flags.
 */
static inline int _spprof_frame_get_owner(void *frame);
```

**Behavior by Version**:
| Version | Shim Detection | Ownership Detection |
|---------|---------------|---------------------|
| 3.9 | N/A (returns 0) | Code flags check |
| 3.10 | N/A (returns 0) | `f_gen_or_coro` field |
| 3.11+ | `owner == CSTACK` | `owner` field |

---

### Pointer Validation

```c
/**
 * Validate pointer is in reasonable address range - ASYNC-SIGNAL-SAFE
 *
 * @param ptr Pointer to validate
 * @return 1 if pointer looks valid, 0 otherwise
 *
 * Catches NULL and obviously invalid addresses. Does NOT guarantee
 * the pointer is actually dereferenceable.
 */
static inline int _spprof_ptr_valid(const void *ptr);
```

---

## Frame Capture API

### Primary Capture Function

```c
/**
 * Capture Python frame code pointers - ASYNC-SIGNAL-SAFE
 *
 * This is the primary function called from signal handlers.
 *
 * @param frames Output array for code object pointers
 * @param max_frames Maximum frames to capture
 * @return Number of frames captured
 *
 * FREE-THREADING WARNING: Not safe in signal handlers on free-threaded
 * builds (Py_GIL_DISABLED) without thread suspension.
 */
int _spprof_capture_frames_unsafe(uintptr_t *frames, int max_frames);
```

### Extended Capture with Instruction Pointers

```c
/**
 * Capture frames with instruction pointers - ASYNC-SIGNAL-SAFE
 *
 * @param code_ptrs Output array for code object pointers
 * @param instr_ptrs Output array for instruction pointers (parallel)
 * @param max_frames Maximum frames to capture
 * @return Number of frames captured
 */
int _spprof_capture_frames_with_instr_unsafe(
    uintptr_t *code_ptrs,
    uintptr_t *instr_ptrs,
    int max_frames
);
```

### Thread-Targeted Capture (for Mach sampler)

```c
/**
 * Capture frames from a specific thread state - NOT async-signal-safe
 *
 * Used by Mach sampler where target thread is suspended.
 * Safe for free-threaded builds when target is suspended.
 *
 * @param tstate Thread state to capture from
 * @param frames Output array for code object pointers
 * @param max_frames Maximum frames to capture
 * @return Number of frames captured
 */
int _spprof_capture_frames_from_tstate(
    PyThreadState *tstate,
    uintptr_t *frames,
    int max_frames
);
```

---

## Compile-Time Configuration

### Removed Flags

```c
/* REMOVED: No longer used */
// #define SPPROF_USE_INTERNAL_API
```

### Version Detection

```c
/* Automatically defined based on Python version */
#define SPPROF_PY39   /* Python 3.9.x */
#define SPPROF_PY310  /* Python 3.10.x */
#define SPPROF_PY311  /* Python 3.11.x */
#define SPPROF_PY312  /* Python 3.12.x */
#define SPPROF_PY313  /* Python 3.13.x */
#define SPPROF_PY314  /* Python 3.14+ */
```

### Free-Threading Detection

```c
#ifdef Py_GIL_DISABLED
    #define SPPROF_FREE_THREADED 1
#else
    #define SPPROF_FREE_THREADED 0
#endif
```

---

## Error Handling

All functions handle NULL inputs gracefully:
- Frame accessors return NULL/0/-1 for NULL input
- Capture functions return 0 frames if thread state is NULL
- Pointer validation returns 0 for NULL pointers

---

## Backward Compatibility

### Breaking Changes

1. **Compile flag removed**: `SPPROF_USE_INTERNAL_API` no longer has effect
2. **Header changes**: `compat/*.h` headers removed
3. **API consolidation**: Single code path for all versions

### Migration Path

Existing code using the internal API mode requires no changes. Code relying on public API fallback must:
1. Ensure Python 3.9+ is the minimum version
2. Update any direct usage of `compat_*` functions to use `_spprof_*` functions

---

## Safety Matrix

| Context | Python 3.9-3.12 | Python 3.13+ GIL | Python 3.13+ Free-threaded |
|---------|-----------------|------------------|---------------------------|
| Signal handler | ✅ Safe | ✅ Safe | ⚠️ Use thread suspension |
| Regular thread | ✅ Safe | ✅ Safe | ✅ Safe with suspension |
| GIL held | ✅ Safe | ✅ Safe | N/A |


