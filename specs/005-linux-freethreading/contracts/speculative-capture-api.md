# Contract: Speculative Capture API

**Feature**: 005-linux-freethreading  
**Date**: December 2, 2024  
**Type**: Internal C API

## Overview

This contract defines the C functions for speculative frame capture on free-threaded Python builds. These functions are called from the SIGPROF signal handler and must be async-signal-safe.

---

## Functions

### `_spprof_speculative_init`

Initialize validation state for speculative capture.

```c
/**
 * Initialize speculative capture validation state.
 * 
 * MUST be called during module initialization (with GIL held).
 * Caches PyCode_Type pointer and sets heap bounds.
 * 
 * Thread Safety: NOT thread-safe. Call once during init.
 * Signal Safety: NOT async-signal-safe. Do not call from handler.
 * 
 * @return 0 on success, -1 on failure
 */
int _spprof_speculative_init(void);
```

**Preconditions**:
- GIL is held
- Called exactly once during `PyInit__native()`

**Postconditions**:
- `g_cached_code_type` is set to `&PyCode_Type`
- `g_speculative_initialized` is 1

---

### `_spprof_capture_frames_speculative`

Capture frames with full validation (for free-threaded builds).

```c
/**
 * Capture Python frames speculatively with validation.
 * 
 * For use in signal handlers on free-threaded Python builds.
 * Validates each pointer before dereferencing and detects cycles.
 * 
 * Thread Safety: Safe (no shared mutable state).
 * Signal Safety: ASYNC-SIGNAL-SAFE.
 * 
 * @param frames     Output array for code object pointers
 * @param max_depth  Maximum frames to capture (must be <= SPPROF_MAX_STACK_DEPTH)
 * @return           Number of valid frames captured, or 0 if validation failed
 */
int _spprof_capture_frames_speculative(
    uintptr_t *frames,
    int max_depth
);
```

**Preconditions**:
- `_spprof_speculative_init()` was called
- `frames` is valid stack-allocated array
- `max_depth > 0 && max_depth <= 128`

**Postconditions**:
- `frames[0..return_value-1]` contain validated code object pointers
- On validation failure, returns 0 and increments drop counter

**Error Handling**:
- Invalid tstate: return 0 (no counter increment)
- Validation failure: return 0, increment `g_samples_dropped_validation`
- Cycle detected: return 0, increment `g_samples_dropped_validation`

---

### `_spprof_capture_frames_with_instr_speculative`

Capture frames with instruction pointers for line number resolution.

```c
/**
 * Capture Python frames with instruction pointers, speculatively.
 * 
 * Same as _spprof_capture_frames_speculative but also captures
 * instruction pointers for accurate line number resolution.
 * 
 * Thread Safety: Safe (no shared mutable state).
 * Signal Safety: ASYNC-SIGNAL-SAFE.
 * 
 * @param code_ptrs   Output array for code object pointers
 * @param instr_ptrs  Output array for instruction pointers (parallel)
 * @param max_depth   Maximum frames to capture
 * @return            Number of valid frames captured
 */
int _spprof_capture_frames_with_instr_speculative(
    uintptr_t *code_ptrs,
    uintptr_t *instr_ptrs,
    int max_depth
);
```

**Preconditions**: Same as `_spprof_capture_frames_speculative`

**Postconditions**:
- `code_ptrs` and `instr_ptrs` are filled in parallel
- `instr_ptrs[i]` may be 0 if instruction pointer unavailable

---

### `_spprof_ptr_valid_speculative`

Fast pointer validation for use in capture path.

```c
/**
 * Validate pointer is within reasonable heap bounds and aligned.
 * 
 * Thread Safety: Safe (pure function).
 * Signal Safety: ASYNC-SIGNAL-SAFE.
 * 
 * @param ptr  Pointer to validate
 * @return     1 if valid, 0 if invalid
 */
static inline int _spprof_ptr_valid_speculative(const void *ptr);
```

**Checks performed**:
1. `ptr != NULL`
2. `(uintptr_t)ptr >= 0x10000` (above null page)
3. `(uintptr_t)ptr <= 0x7FFFFFFFFFFF` (user-space limit)
4. `((uintptr_t)ptr & 0x7) == 0` (8-byte aligned)

---

### `_spprof_looks_like_code`

Validate object appears to be a PyCodeObject.

```c
/**
 * Check if object looks like a PyCodeObject.
 * 
 * Compares ob_type to cached PyCode_Type pointer.
 * 
 * Thread Safety: Safe (reads immutable cached data).
 * Signal Safety: ASYNC-SIGNAL-SAFE.
 * 
 * @param obj  Object to check (must have passed ptr_valid)
 * @return     1 if looks like code object, 0 otherwise
 */
static inline int _spprof_looks_like_code(PyObject *obj);
```

**Preconditions**:
- `_spprof_ptr_valid_speculative(obj)` returned 1
- `_spprof_speculative_init()` was called

---

### `_spprof_speculative_dropped_count`

Get count of samples dropped due to validation failure.

```c
/**
 * Get number of samples dropped due to validation failure.
 * 
 * Thread Safety: Safe (atomic read).
 * Signal Safety: NOT async-signal-safe (not needed in handler).
 * 
 * @return Number of dropped samples
 */
uint64_t _spprof_speculative_dropped_count(void);
```

---

## Architecture-Specific Macros

### `SPPROF_ATOMIC_LOAD_PTR`

Architecture-appropriate pointer load with memory ordering.

```c
#if defined(__aarch64__)
    /* ARM64: Weak memory model requires acquire barrier */
    #define SPPROF_ATOMIC_LOAD_PTR(ptr) \
        __atomic_load_n((void**)(ptr), __ATOMIC_ACQUIRE)
#else
    /* x86-64: Strong memory model, plain load sufficient */
    #define SPPROF_ATOMIC_LOAD_PTR(ptr) (*(void**)(ptr))
#endif
```

---

## Constants

```c
/* Heap bounds for 64-bit systems */
#define SPPROF_HEAP_LOWER_BOUND  ((uintptr_t)0x10000)
#define SPPROF_HEAP_UPPER_BOUND  ((uintptr_t)0x00007FFFFFFFFFFF)

/* Cycle detection window size */
#define SPPROF_CYCLE_WINDOW_SIZE 8

/* Tagged pointer mask for Python 3.14 _PyStackRef */
#define SPPROF_STACKREF_TAG_MASK ((uintptr_t)0x3)
```

---

## Usage Example

```c
/* In signal_handler.c */

static inline int
capture_python_stack_unsafe(uintptr_t* frames, int max_depth) {
#if SPPROF_FREE_THREADED && defined(__linux__)
    /* Free-threaded Linux: Use speculative capture with validation */
    return _spprof_capture_frames_speculative(frames, max_depth);
#elif SPPROF_FREE_THREADED && defined(__APPLE__)
    /* Free-threaded macOS: Use Mach sampler (handled elsewhere) */
    return 0;  /* Not called on Darwin */
#else
    /* GIL-enabled: Use direct capture (existing code) */
    return _spprof_capture_frames_unsafe(frames, max_depth);
#endif
}
```

---

## Error Codes

This API does not use error codes. Functions return:
- `0`: No frames captured (empty stack or validation failure)
- `>0`: Number of valid frames captured

Validation failures are tracked via atomic counter, accessible via `_spprof_speculative_dropped_count()`.

---

## Revision History

| Version | Date | Changes |
|---------|------|---------|
| 1.0.0 | 2024-12-02 | Initial contract |

