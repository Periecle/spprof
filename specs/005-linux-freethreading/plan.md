# Implementation Plan: Linux Free-Threading Support

**Branch**: `005-linux-freethreading` | **Date**: December 2, 2024 | **Spec**: [spec.md](spec.md)  
**Input**: Feature specification from `/specs/005-linux-freethreading/spec.md`

## Summary

Enable Python profiling on free-threaded Python 3.13t and 3.14t builds on Linux by implementing **Speculative Reading with Validation**. The approach reads frame chain pointers speculatively during SIGPROF signal handling, validates each pointer (heap bounds, alignment, type check), detects cycles, and gracefully drops corrupted samples rather than crashing. Supports both x86-64 (strong memory model) and ARM64 (requires acquire barriers).

## Technical Context

**Language/Version**: C11 (extension), Python 3.13t/3.14t (free-threaded)  
**Primary Dependencies**: None beyond Python C API; uses platform intrinsics for memory barriers  
**Storage**: N/A (in-memory ring buffer, existing infrastructure)  
**Testing**: pytest for integration tests; stress tests for race condition validation  
**Target Platform**: Linux x86-64 and ARM64  
**Project Type**: Single project (C extension with Python bindings)  
**Performance Goals**: ~500ns overhead per sample; <2x overhead vs GIL-enabled builds  
**Constraints**: Must be async-signal-safe; no heap allocation in signal handler; no Python API calls in capture path  
**Scale/Scope**: Existing codebase modification; ~200-400 lines of new C code

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Notes |
|-----------|--------|-------|
| **I. Minimal Overhead** | ✅ PASS | ~500ns per sample overhead; speculative reads avoid thread coordination cost |
| **II. Memory Safety & Stability** | ✅ PASS | Validation catches invalid pointers; graceful drop on corruption; no crashes |
| **III. Cross-Platform Portability** | ✅ PASS | Linux-specific but isolated in `signal_handler.c` and `pycore_tstate.h`; macOS already uses Mach sampler |
| **IV. Statistical Accuracy** | ✅ PASS | ~99.9% sample validity expected; bias is negligible due to tiny race window |
| **V. Clean C-Python Boundary** | ✅ PASS | C handles capture/validation; Python handles stats reporting via existing API |

**No constitution violations detected.** All changes align with established principles.

## Project Structure

### Documentation (this feature)

```text
specs/005-linux-freethreading/
├── plan.md              # This file
├── research.md          # Phase 0: Technical research
├── data-model.md        # Phase 1: Data structures
├── quickstart.md        # Phase 1: Implementation guide
├── contracts/           # Phase 1: Internal C API contracts
│   └── speculative-capture-api.md
└── tasks.md             # Phase 2: Implementation tasks (created by /speckit.tasks)
```

### Source Code (repository root)

```text
src/spprof/_ext/
├── internal/
│   ├── pycore_frame.h       # MODIFY: Add SPPROF_FREE_THREADING_SAFE for Linux
│   └── pycore_tstate.h      # MODIFY: Add speculative capture functions
├── signal_handler.c         # MODIFY: Use speculative capture for free-threaded
├── signal_handler.h         # MODIFY: Add validation statistics accessors
└── module.c                 # MODIFY: Remove startup block for free-threaded Linux

tests/
├── test_freethreading.py    # NEW: Free-threading specific tests
└── test_stress.py           # MODIFY: Add free-threading stress scenarios
```

**Structure Decision**: Modifications to existing C extension files in `src/spprof/_ext/`. No new directories needed. All changes isolated to signal-handler capture path.

## Complexity Tracking

> No constitution violations requiring justification.

| Violation | Why Needed | Simpler Alternative Rejected Because |
|-----------|------------|-------------------------------------|
| N/A | N/A | N/A |
