# Quickstart: Internal API Only Implementation

**Feature**: 004-internal-api-only  
**Date**: 2025-12-01

## Overview

This guide provides step-by-step instructions for implementing the internal-API-only frame walking, removing public API reliance across all Python versions (3.9-3.14).

---

## Prerequisites

- Python development headers installed for target versions
- C compiler supporting C11 or later
- Understanding of Python frame structures (see `data-model.md`)
- Familiarity with async-signal-safe programming

---

## Implementation Steps

### Step 1: Update Version Detection

Add Python 3.9 and 3.10 detection macros to `internal/pycore_frame.h`:

```c
/* Add to version detection section */
#define SPPROF_PY39  (PY_VERSION_HEX >= 0x03090000 && PY_VERSION_HEX < 0x030A0000)
#define SPPROF_PY310 (PY_VERSION_HEX >= 0x030A0000 && PY_VERSION_HEX < 0x030B0000)

/* Update minimum version check */
#if PY_VERSION_HEX < 0x03090000
    #error "spprof requires Python 3.9 or later"
#endif
```

### Step 2: Add Python 3.9 Frame Structure

Add to `internal/pycore_frame.h`:

```c
#if SPPROF_PY39

typedef struct _spprof_PyFrameObject {
    PyObject_VAR_HEAD
    struct _spprof_PyFrameObject *f_back;
    PyCodeObject *f_code;
    PyObject *f_builtins;
    PyObject *f_globals;
    PyObject *f_locals;
    PyObject **f_valuestack;
    PyObject **f_stacktop;
    PyObject *f_trace;
    char f_trace_lines;
    char f_trace_opcodes;
    PyObject *f_gen;
    int f_lasti;
    int f_lineno;
    int f_iblock;
    char f_executing;
    PyTryBlock f_blockstack[CO_MAXBLOCKS];
    PyObject *f_localsplus[1];
} _spprof_PyFrameObject;

static inline _spprof_PyFrameObject*
_spprof_get_current_frame(PyThreadState *tstate) {
    if (tstate == NULL) return NULL;
    return (_spprof_PyFrameObject *)tstate->frame;
}

/* ... other accessor functions ... */

#endif /* SPPROF_PY39 */
```

### Step 3: Add Python 3.10 Frame Structure

Add to `internal/pycore_frame.h`:

```c
#if SPPROF_PY310

typedef struct _spprof_PyFrameObject {
    PyObject_VAR_HEAD
    struct _spprof_PyFrameObject *f_back;
    PyCodeObject *f_code;
    PyObject *f_builtins;
    PyObject *f_globals;
    PyObject *f_locals;
    PyObject **f_valuestack;
    PyObject *f_trace;
    int f_stackdepth;
    char f_trace_lines;
    char f_trace_opcodes;
    char f_gen_or_coro;
    PyFrameState f_state;
    int f_lasti;
    int f_lineno;
} _spprof_PyFrameObject;

static inline _spprof_PyFrameObject*
_spprof_get_current_frame(PyThreadState *tstate) {
    if (tstate == NULL) return NULL;
    return (_spprof_PyFrameObject *)tstate->frame;
}

/* ... other accessor functions ... */

#endif /* SPPROF_PY310 */
```

### Step 4: Update Thread State Access

Update `internal/pycore_tstate.h` for 3.9/3.10:

```c
static inline PyThreadState*
_spprof_tstate_get(void) {
#if PY_VERSION_HEX >= 0x030D0000  /* 3.13+ */
    return PyThreadState_GetUnchecked();
#else
    /* 3.9-3.12: PyThreadState_GET() is async-signal-safe */
    return PyThreadState_GET();
#endif
}
```

### Step 5: Update Frame Capture Functions

Update `internal/pycore_tstate.h` to handle all versions:

```c
static inline int
_spprof_capture_frames_unsafe(uintptr_t *frames, int max_frames) {
    if (frames == NULL || max_frames <= 0) return 0;

    PyThreadState *tstate = _spprof_tstate_get();
    if (!_spprof_ptr_valid(tstate)) return 0;

#if SPPROF_PY39 || SPPROF_PY310
    _spprof_PyFrameObject *frame = _spprof_get_current_frame(tstate);
#else
    _spprof_InterpreterFrame *frame = _spprof_get_current_frame(tstate);
#endif

    int count = 0;
    int safety_limit = SPPROF_FRAME_WALK_LIMIT;

    while (frame != NULL && count < max_frames && safety_limit-- > 0) {
        if (!_spprof_ptr_valid(frame)) break;

#if SPPROF_PY311 || SPPROF_PY312 || SPPROF_PY313 || SPPROF_PY314
        if (_spprof_frame_is_shim(frame)) {
            frame = _spprof_frame_get_previous(frame);
            continue;
        }
#endif

        PyCodeObject *code = _spprof_frame_get_code(frame);
        if (_spprof_ptr_valid(code)) {
            frames[count++] = (uintptr_t)code;
        }

        frame = _spprof_frame_get_previous(frame);
    }

    return count;
}
```

### Step 6: Remove Public API Mode from framewalker.c

Update `framewalker.c` to remove the conditional compilation:

```c
/* BEFORE: Remove this block */
#ifdef SPPROF_USE_INTERNAL_API
    /* Production mode: async-signal-safe internal API */
    #include "internal/pycore_frame.h"
    ...
#else
    /* Fallback mode: public API with version dispatch */
    #include "compat/py311.h"
    ...
#endif

/* AFTER: Always use internal API */
#include "internal/pycore_frame.h"
#include "internal/pycore_tstate.h"
```

### Step 7: Delete Compatibility Headers

Remove the following files:
- `src/spprof/_ext/compat/py39.h`
- `src/spprof/_ext/compat/py311.h`
- `src/spprof/_ext/compat/py312.h`
- `src/spprof/_ext/compat/py313.h`
- `src/spprof/_ext/compat/py314.h`

### Step 8: Update Build Configuration

Update `setup.py` or `pyproject.toml` to remove `SPPROF_USE_INTERNAL_API` from compile definitions:

```python
# BEFORE
ext_modules = [
    Extension(
        "_native",
        sources=sources,
        define_macros=[("SPPROF_USE_INTERNAL_API", "1")],  # Remove this
    )
]

# AFTER
ext_modules = [
    Extension(
        "_native",
        sources=sources,
        # No special macros needed - internal API is default
    )
]
```

### Step 9: Update module.c Initialization

Ensure version info reflects internal API usage:

```c
/* Update version info string */
snprintf(g_version_info, sizeof(g_version_info),
         "internal-api (Python %d.%d.%d)",
         PY_MAJOR_VERSION, PY_MINOR_VERSION, PY_MICRO_VERSION);
```

### Step 10: Add Struct Validation (Optional but Recommended)

Add compile-time validation in `internal/pycore_frame.h`:

```c
/* Validate critical struct sizes and offsets */
#if SPPROF_PY311
_Static_assert(
    offsetof(_spprof_InterpreterFrame, f_code) == 32,
    "Python 3.11 f_code offset mismatch"
);
_Static_assert(
    offsetof(_spprof_InterpreterFrame, previous) == 48,
    "Python 3.11 previous offset mismatch"
);
#endif
```

---

## Testing Checklist

### Unit Tests

- [ ] Frame capture returns correct frame count on 3.9
- [ ] Frame capture returns correct frame count on 3.10
- [ ] Frame capture returns correct frame count on 3.11+
- [ ] Instruction pointer is non-NULL for all versions
- [ ] Frame ownership detected correctly for generators
- [ ] Shim frames skipped on 3.11+

### Integration Tests

- [ ] Profiler works end-to-end on Python 3.9
- [ ] Profiler works end-to-end on Python 3.10
- [ ] Profiler works end-to-end on Python 3.11
- [ ] Profiler works end-to-end on Python 3.12
- [ ] Profiler works end-to-end on Python 3.13
- [ ] Profiler works end-to-end on Python 3.14

### Stress Tests

- [ ] No crashes at 1000 samples/sec on 3.9
- [ ] No crashes at 1000 samples/sec on 3.10
- [ ] Memory stable over extended profiling sessions

### Compatibility Tests

- [ ] Compiles on macOS (Darwin)
- [ ] Compiles on Linux
- [ ] Compiles on Windows
- [ ] Works with Python debug builds

---

## Common Issues

### Issue: `PyTryBlock` undefined on Python 3.11+

**Solution**: Python 3.11 removed `PyTryBlock`. The struct definition for 3.9 should include:

```c
#if SPPROF_PY39
/* Include for PyTryBlock definition */
#include <frameobject.h>
#endif
```

### Issue: `tstate->frame` not available on Python 3.11+

**Solution**: Ensure version-specific access:

```c
#if SPPROF_PY39 || SPPROF_PY310
    return (_spprof_PyFrameObject *)tstate->frame;
#else
    /* Use cframe or current_frame based on version */
#endif
```

### Issue: `f_lasti` returns unexpected values

**Solution**: In Python 3.10, `f_lasti` semantic changed. Verify bytecode offset calculation:

```c
/* Python 3.9: f_lasti is byte offset */
/* Python 3.10: f_lasti is also byte offset but word-aligned */
void *instr = (void*)(PyBytes_AS_STRING(code->co_code) + frame->f_lasti);
```

### Issue: Struct size mismatch on different platforms

**Solution**: Add platform-specific padding checks:

```c
#if defined(__LP64__) || defined(_WIN64)
    /* 64-bit platform: verify 8-byte pointer sizes */
#else
    /* 32-bit platform: adjust struct layouts */
#endif
```

---

## Verification Commands

```bash
# Build and test on specific Python version
python3.9 -m pip install -e . && python3.9 -m pytest tests/

# Run stress test
python3.9 -c "
import spprof
import threading

def work():
    sum(range(10000000))

with spprof.Profiler() as p:
    threads = [threading.Thread(target=work) for _ in range(4)]
    for t in threads: t.start()
    for t in threads: t.join()

print(p.stats())
"
```

---

## Next Steps

After implementation:
1. Run full test suite on all Python versions
2. Update documentation to remove public API references
3. Update CI to test 3.9 and 3.10 with internal API
4. Consider adding `offsetof` validation for struct fields

