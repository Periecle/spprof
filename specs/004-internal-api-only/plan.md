# Implementation Plan: Internal API Only Frame Walking

**Branch**: `004-internal-api-only` | **Date**: 2025-12-01 | **Spec**: [spec.md](./spec.md)  
**Input**: Feature specification from `/specs/004-internal-api-only/spec.md`

## Summary

Remove reliance on Python's public C API for frame walking and use internal struct access exclusively across all Python versions (3.9-3.14). This provides:
- Consistent frame details (code objects, instruction pointers, ownership) on all versions
- Async-signal-safe frame capture (no `Py_DECREF` in signal handlers)
- Simplified codebase with single implementation path

## Technical Context

**Language/Version**: C11 with Python 3.9-3.14 support  
**Primary Dependencies**: Python C API, pthread (POSIX), Mach API (Darwin)  
**Storage**: N/A  
**Testing**: pytest with C extension tests  
**Target Platform**: Linux (x86_64, aarch64), macOS (arm64, x86_64), Windows  
**Project Type**: Single project (C extension module)  
**Performance Goals**: Frame capture < 1μs, 1000+ samples/second without crashes  
**Constraints**: Async-signal-safe frame walking, no memory allocation in signal handlers  
**Scale/Scope**: Support Python 3.9-3.14, ~6 version-specific struct definitions

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

The project constitution is template-based and does not define specific constraints. Implementation follows:
- ✅ Self-contained library design
- ✅ Testable independently per Python version
- ✅ Clear purpose: frame walking for sampling profiler

## Project Structure

### Documentation (this feature)

```text
specs/004-internal-api-only/
├── plan.md              # This file
├── research.md          # Phase 0: Python version frame structure research
├── data-model.md        # Phase 1: Struct definitions for all versions
├── quickstart.md        # Phase 1: Implementation guide
├── contracts/           # Phase 1: API contracts
│   └── internal-frame-api.md
└── tasks.md             # Phase 2: Implementation tasks (via /speckit.tasks)
```

### Source Code (repository root)

```text
src/spprof/_ext/
├── internal/                    # MODIFIED: Unified internal headers
│   ├── pycore_frame.h          # Frame structs for 3.9-3.14
│   └── pycore_tstate.h         # Thread state + capture functions
├── compat/                      # DELETED: Remove public API compat headers
│   ├── py39.h                  # DELETE
│   ├── py311.h                 # DELETE
│   ├── py312.h                 # DELETE
│   ├── py313.h                 # DELETE
│   └── py314.h                 # DELETE
├── framewalker.c               # MODIFIED: Remove public API code path
├── framewalker.h               # UNCHANGED
├── module.c                    # MODIFIED: Update version info
└── ...                         # Other files unchanged

tests/
├── test_version_compat.py      # MODIFIED: Test 3.9/3.10 internal API
├── test_profiler.py            # UNCHANGED
└── ...
```

**Structure Decision**: Single C extension module with version-specific struct definitions in `internal/` headers. No changes to overall project structure.

## Phase 0: Research Summary

Research completed in `research.md`. Key findings:

| Topic | Decision |
|-------|----------|
| Python 3.9/3.10 frames | Use direct `PyFrameObject` struct access via `tstate->frame` |
| Instruction pointer | Compute from `f_lasti` offset for 3.9/3.10; use pointer for 3.11+ |
| Frame ownership | Infer from code flags (3.9) or `f_gen_or_coro` (3.10); use `owner` field (3.11+) |
| Header approach | Self-contained struct definitions validated against CPython source |
| Thread state access | Version-specific: `tstate->frame` (3.9/10), `cframe` (3.11/12), `current_frame` (3.13+) |

## Phase 1: Design Summary

### Data Model

Six frame structure variants defined in `data-model.md`:

1. **Python 3.9**: `_spprof_PyFrameObject` - classic PyFrameObject layout
2. **Python 3.10**: `_spprof_PyFrameObject` - modified with `f_gen_or_coro`, `f_state`
3. **Python 3.11**: `_spprof_InterpreterFrame` - with CFrame linkage
4. **Python 3.12**: `_spprof_InterpreterFrame` - CFrame still present
5. **Python 3.13**: `_spprof_InterpreterFrame` - direct `current_frame` in tstate
6. **Python 3.14**: `_spprof_InterpreterFrame` - with `_PyStackRef` tagged pointers

### API Contracts

Unified API defined in `contracts/internal-frame-api.md`:

| Function | Purpose |
|----------|---------|
| `_spprof_tstate_get()` | Get current thread state (async-signal-safe) |
| `_spprof_get_current_frame()` | Get current frame from tstate |
| `_spprof_frame_get_previous()` | Navigate to caller frame |
| `_spprof_frame_get_code()` | Get PyCodeObject from frame |
| `_spprof_frame_get_instr_ptr()` | Get instruction pointer |
| `_spprof_frame_is_shim()` | Check for C-stack shim frame |
| `_spprof_frame_get_owner()` | Get frame ownership type |
| `_spprof_capture_frames_unsafe()` | Capture frames (signal handler) |
| `_spprof_capture_frames_from_tstate()` | Capture from specific thread |

### Implementation Guide

`quickstart.md` provides:
- Step-by-step implementation instructions
- Code snippets for each modification
- Testing checklist
- Common issues and solutions

## Key Implementation Changes

### Files to Modify

| File | Changes |
|------|---------|
| `internal/pycore_frame.h` | Add 3.9/3.10 struct definitions, update version macros |
| `internal/pycore_tstate.h` | Update capture functions for 3.9/3.10 |
| `framewalker.c` | Remove `#ifdef SPPROF_USE_INTERNAL_API` branches |
| `module.c` | Update version info string |
| `setup.py` | Remove `SPPROF_USE_INTERNAL_API` define |

### Files to Delete

| File | Reason |
|------|--------|
| `compat/py39.h` | Public API fallback no longer needed |
| `compat/py311.h` | Public API fallback no longer needed |
| `compat/py312.h` | Public API fallback no longer needed |
| `compat/py313.h` | Public API fallback no longer needed |
| `compat/py314.h` | Public API fallback no longer needed |

### New Code (LOC Estimate)

| Component | Lines |
|-----------|-------|
| Python 3.9 struct + accessors | ~80 |
| Python 3.10 struct + accessors | ~70 |
| Updates to pycore_tstate.h | ~50 |
| **Total new code** | ~200 |

### Removed Code (LOC Estimate)

| Component | Lines |
|-----------|-------|
| compat/py39.h | ~80 |
| compat/py311.h | ~70 |
| compat/py312.h | ~70 |
| compat/py313.h | ~75 |
| compat/py314.h | ~70 |
| framewalker.c public API branch | ~100 |
| **Total removed code** | ~465 |

**Net reduction**: ~265 lines of code

## Risk Assessment

| Risk | Probability | Impact | Mitigation |
|------|-------------|--------|------------|
| Struct layout mismatch on minor version | Low | High | Add compile-time `offsetof` validation |
| Python debug builds have different layout | Medium | Medium | Document supported build configurations |
| ABI mismatch when reusing extension | Low | High | Encode Python version in module filename (already done) |
| Free-threading crashes on 3.13+ | Low | High | Existing thread suspension logic handles this |

## Testing Strategy

### Unit Tests (per Python version)

- Frame capture returns correct depth
- Instruction pointers non-NULL
- Code objects match expected functions
- Ownership detection for generators/coroutines

### Integration Tests

- End-to-end profiling on all versions
- Multi-threaded profiling
- Generator/coroutine profiling

### Stress Tests

- 1000 samples/second for 60 seconds
- Memory leak detection with Valgrind
- No SIGSEGV/SIGBUS in signal handler

### CI Matrix

```yaml
python-version: [3.9, 3.10, 3.11, 3.12, 3.13, 3.14]
os: [ubuntu-latest, macos-latest, windows-latest]
```

## Complexity Tracking

No constitution violations. Implementation follows single-project pattern with version-specific code paths encapsulated in headers.

## Next Steps

1. **Create Tasks**: Run `/speckit.tasks` to break this plan into implementation tasks
2. **Implement**: Follow `quickstart.md` step-by-step
3. **Test**: Run full test suite on all Python versions
4. **CI Update**: Ensure CI tests 3.9/3.10 with the new internal API path

---

## Generated Artifacts

| Artifact | Path | Status |
|----------|------|--------|
| Research | `research.md` | ✅ Complete |
| Data Model | `data-model.md` | ✅ Complete |
| API Contract | `contracts/internal-frame-api.md` | ✅ Complete |
| Quickstart | `quickstart.md` | ✅ Complete |
| Tasks | `tasks.md` | ⏳ Pending `/speckit.tasks` |
