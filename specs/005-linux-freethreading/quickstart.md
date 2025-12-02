# Quickstart: Linux Free-Threading Implementation

**Feature**: 005-linux-freethreading  
**Date**: December 2, 2024

## Prerequisites

- Linux system (x86-64 or ARM64)
- Python 3.13t or 3.14t (free-threaded build with `Py_GIL_DISABLED`)
- C compiler with C11 support (gcc 4.9+, clang 3.6+)
- spprof source code checked out

## Implementation Overview

This feature modifies 4 existing files and adds 1 new test file:

| File | Action | Purpose |
|------|--------|---------|
| `src/spprof/_ext/internal/pycore_frame.h` | Modify | Enable speculative capture flag for Linux |
| `src/spprof/_ext/internal/pycore_tstate.h` | Modify | Add speculative capture functions |
| `src/spprof/_ext/signal_handler.c` | Modify | Use speculative capture, add stats |
| `src/spprof/_ext/module.c` | Modify | Remove startup block, init validation |
| `tests/test_freethreading.py` | Create | Free-threading specific tests |

## Step-by-Step Implementation

### Step 1: Enable Free-Threading Flag for Linux

In `pycore_frame.h`, modify the `SPPROF_FREE_THREADING_SAFE` definition:

```c
/* BEFORE */
#if SPPROF_FREE_THREADED
    #if defined(__APPLE__)
        #define SPPROF_FREE_THREADING_SAFE 1
    #else
        #define SPPROF_FREE_THREADING_SAFE 0
    #endif
#else
    #define SPPROF_FREE_THREADING_SAFE 1
#endif

/* AFTER */
#if SPPROF_FREE_THREADED
    #if defined(__APPLE__) || defined(__linux__)
        /* Darwin uses Mach sampler, Linux uses speculative capture */
        #define SPPROF_FREE_THREADING_SAFE 1
    #else
        #define SPPROF_FREE_THREADING_SAFE 0
    #endif
#else
    #define SPPROF_FREE_THREADING_SAFE 1
#endif
```

### Step 2: Add Speculative Capture to pycore_tstate.h

Add the following after the existing capture functions:

```c
/*
 * =============================================================================
 * Speculative Frame Capture (Free-Threading Safe)
 * =============================================================================
 */

#if SPPROF_FREE_THREADED && defined(__linux__)

/* Cached validation state (set at init, never modified) */
extern PyTypeObject *_spprof_cached_code_type;
extern int _spprof_speculative_initialized;

/* Drop counter for validation failures */
extern _Atomic uint64_t _spprof_samples_dropped_validation;

/* Architecture-specific atomic load */
#if defined(__aarch64__)
    #define SPPROF_ATOMIC_LOAD_PTR(ptr) \
        __atomic_load_n((void**)(ptr), __ATOMIC_ACQUIRE)
#else
    #define SPPROF_ATOMIC_LOAD_PTR(ptr) (*(void**)(ptr))
#endif

/* Initialize speculative capture (call from module init) */
static inline int _spprof_speculative_init(void) {
    _spprof_cached_code_type = &PyCode_Type;
    _spprof_speculative_initialized = 1;
    return 0;
}

/* Enhanced pointer validation */
static inline int _spprof_ptr_valid_speculative(const void *ptr) {
    uintptr_t addr = (uintptr_t)ptr;
    return addr >= 0x10000 
        && addr <= 0x00007FFFFFFFFFFF
        && (addr & 0x7) == 0;
}

/* Type check without Python API */
static inline int _spprof_looks_like_code(PyObject *obj) {
    if (!_spprof_ptr_valid_speculative(obj)) return 0;
    PyTypeObject *type = obj->ob_type;
    return _spprof_ptr_valid_speculative(type) 
        && type == _spprof_cached_code_type;
}

/* Main speculative capture function */
static inline int
_spprof_capture_frames_speculative(uintptr_t *frames, int max_frames) {
    if (!_spprof_speculative_initialized || frames == NULL || max_frames <= 0) {
        return 0;
    }

    PyThreadState *tstate = _spprof_tstate_get();
    if (!_spprof_ptr_valid_speculative(tstate)) {
        return 0;
    }

    int depth = 0;
    uintptr_t seen[8] = {0};
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
        for (int i = 0; i < 8 && i < depth; i++) {
            if (seen[i] == (uintptr_t)frame) {
                atomic_fetch_add_explicit(
                    &_spprof_samples_dropped_validation, 1, 
                    memory_order_relaxed);
                return 0;  /* Cycle detected - drop sample */
            }
        }
        seen[seen_idx++ & 7] = (uintptr_t)frame;

        /* 3. Skip shim frames */
        if (frame->owner == SPPROF_FRAME_OWNED_BY_CSTACK) {
            frame = (_spprof_InterpreterFrame *)
                SPPROF_ATOMIC_LOAD_PTR(&frame->previous);
            continue;
        }

        /* 4. Extract code object (handle tagged pointers for 3.14) */
#if SPPROF_PY314
        PyObject *code = (PyObject *)(frame->f_executable.bits & ~0x3ULL);
#else
        PyObject *code = frame->f_executable;
#endif

        /* 5. Validate code object */
        if (_spprof_looks_like_code(code)) {
            frames[depth++] = (uintptr_t)code;
        }

        /* 6. Move to previous frame with memory ordering */
        frame = (_spprof_InterpreterFrame *)
            SPPROF_ATOMIC_LOAD_PTR(&frame->previous);
    }

    return depth;
}

#endif /* SPPROF_FREE_THREADED && __linux__ */
```

### Step 3: Modify signal_handler.c

Update the capture function selector:

```c
static inline int
capture_python_stack_unsafe(uintptr_t* frames, int max_depth) {
#ifdef SPPROF_USE_INTERNAL_API
    #if SPPROF_FREE_THREADED && defined(__linux__)
        /* Free-threaded Linux: Use speculative capture */
        return _spprof_capture_frames_speculative(frames, max_depth);
    #elif SPPROF_FREE_THREADED
        /* Free-threaded non-Linux: Should use platform sampler */
        return 0;
    #else
        /* GIL-enabled: Use direct capture */
        return _spprof_capture_frames_unsafe(frames, max_depth);
    #endif
#else
    return framewalker_capture_raw(frames, max_depth);
#endif
}
```

Add global variables and statistics accessor:

```c
/* Global validation state (in signal_handler.c) */
#if SPPROF_FREE_THREADED && defined(__linux__)
PyTypeObject *_spprof_cached_code_type = NULL;
int _spprof_speculative_initialized = 0;
_Atomic uint64_t _spprof_samples_dropped_validation = 0;
#endif

/* Statistics accessor */
uint64_t signal_handler_validation_drops(void) {
#if SPPROF_FREE_THREADED && defined(__linux__)
    return atomic_load(&_spprof_samples_dropped_validation);
#else
    return 0;
#endif
}
```

### Step 4: Update module.c

Remove the startup block and add initialization:

```c
/* In PyInit__native() */
#if SPPROF_FREE_THREADED && defined(__linux__)
    if (_spprof_speculative_init() < 0) {
        PyErr_SetString(PyExc_RuntimeError,
            "Failed to initialize speculative capture");
        return NULL;
    }
#endif

/* Remove or modify the error that blocks free-threaded startup */
/* The existing SPPROF_FREE_THREADING_SAFE check will now pass for Linux */
```

### Step 5: Create tests/test_freethreading.py

```python
"""Tests for free-threaded Python support."""
import sys
import threading
import pytest

# Skip entire module if not free-threaded
pytestmark = pytest.mark.skipif(
    not hasattr(sys, '_is_gil_enabled') or sys._is_gil_enabled(),
    reason="Requires free-threaded Python (3.13t+)"
)


def test_basic_profiling_freethreaded():
    """Test that basic profiling works on free-threaded Python."""
    import spprof
    
    def work():
        total = 0
        for i in range(10000):
            total += i
        return total
    
    with spprof.Profiler() as p:
        work()
    
    stats = p.stats()
    assert stats['samples_captured'] > 0


def test_multithreaded_profiling():
    """Test profiling with multiple concurrent threads."""
    import spprof
    
    results = []
    
    def worker(n):
        total = 0
        for i in range(10000):
            total += i * n
        results.append(total)
    
    with spprof.Profiler() as p:
        threads = [threading.Thread(target=worker, args=(i,)) for i in range(4)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
    
    stats = p.stats()
    assert stats['samples_captured'] > 0
    assert len(results) == 4


def test_validation_drops_tracked():
    """Test that validation drops are tracked in statistics."""
    import spprof
    
    with spprof.Profiler() as p:
        # Normal workload - should have minimal drops
        for _ in range(100):
            sum(range(1000))
    
    stats = p.stats()
    # Drops should be countable (even if 0)
    assert 'samples_dropped' in stats or 'validation_drops' in stats


def test_no_crash_under_contention():
    """Stress test: no crashes under high thread contention."""
    import spprof
    
    stop_flag = threading.Event()
    
    def churner():
        """Rapidly create and destroy stack frames."""
        while not stop_flag.is_set():
            def a(): return b()
            def b(): return c()
            def c(): return 42
            a()
    
    with spprof.Profiler(interval_ms=1) as p:  # Fast sampling
        threads = [threading.Thread(target=churner) for _ in range(8)]
        for t in threads:
            t.start()
        
        # Let it run for a bit
        import time
        time.sleep(0.5)
        
        stop_flag.set()
        for t in threads:
            t.join()
    
    # If we got here without crashing, test passed
    stats = p.stats()
    assert stats['samples_captured'] >= 0  # Just verify we can read stats
```

## Verification

### Build and Test

```bash
# Build the extension
pip install -e .

# Run free-threading tests (requires Python 3.13t/3.14t)
pytest tests/test_freethreading.py -v

# Run stress tests
pytest tests/test_stress.py -v -k freethreading
```

### Manual Verification

```python
import sys
print(f"GIL enabled: {sys._is_gil_enabled()}")  # Should be False

import spprof

def recursive(n):
    if n <= 0:
        return 0
    return n + recursive(n - 1)

with spprof.Profiler() as p:
    recursive(100)

print(p.stats())
# Should show samples_captured > 0
```

## Troubleshooting

### "Profiler not supported on free-threaded Python"

This error means `SPPROF_FREE_THREADING_SAFE` is still 0. Verify:
1. You're on Linux (not Windows)
2. The `pycore_frame.h` change is applied
3. The extension was rebuilt after changes

### Zero samples captured

Check:
1. `_spprof_speculative_init()` was called (add debug print)
2. `_spprof_cached_code_type` is not NULL
3. Signal handler is being invoked (check `samples_dropped` too)

### High drop rate (>1%)

This is unexpected under normal conditions. Check:
1. ARM64: Verify acquire barriers are being used
2. Memory pressure: System may be aggressively reclaiming
3. Profiling very short-lived threads

