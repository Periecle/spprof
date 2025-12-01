# Implementation Plan: Resolve TODOs, Race Conditions, and Incomplete Implementations

**Branch**: `002-resolve-todos-cleanup` | **Date**: 2025-12-01 | **Spec**: [spec.md](./spec.md)
**Input**: Feature specification from `/specs/002-resolve-todos-cleanup/spec.md`

## Summary

This feature addresses technical debt in the spprof sampling profiler by:
1. Wiring up dropped sample counts from the native ring buffer to the Python API
2. Implementing overhead percentage estimation based on profiling metrics
3. Fixing signal delivery race conditions in the macOS implementation
4. Removing leftover backup files from the repository

## Technical Context

**Language/Version**: C (C11 for atomics), Python 3.9-3.14  
**Primary Dependencies**: POSIX signals, pthread, ringbuffer.c, signal_handler.c  
**Storage**: N/A (in-memory ring buffer)  
**Testing**: pytest, existing tests in `tests/test_platform.py`, `tests/test_profiler.py`  
**Target Platform**: Linux, macOS, Windows  
**Project Type**: Single library with C extension  
**Performance Goals**: Overhead estimation should itself be < 1% overhead  
**Constraints**: Signal handler code must remain async-signal-safe  
**Scale/Scope**: Changes to ~5 files; <200 LOC modified

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

**Note**: Project constitution is a template without project-specific rules defined. Proceeding with standard quality gates:

| Gate | Status | Notes |
|------|--------|-------|
| Existing tests must pass | ✅ | Will run `pytest tests/` before merging |
| No new async-signal-safety violations | ✅ | No changes to signal handler code |
| Cross-platform compatibility | ✅ | macOS changes use POSIX APIs; Windows unchanged |

## Project Structure

### Documentation (this feature)

```text
specs/002-resolve-todos-cleanup/
├── plan.md              # This file
├── research.md          # Signal draining research
├── data-model.md        # N/A (no new data structures)
├── quickstart.md        # Validation steps
├── contracts/           # N/A (internal API only)
└── tasks.md             # Implementation tasks (via /speckit.tasks)
```

### Source Code (repository root)

```text
src/
├── spprof/
│   ├── __init__.py          # Python API (US1: stats wiring)
│   └── _ext/
│       └── platform/
│           └── darwin.c     # macOS platform (US2: race fix)

# Files to delete:
src/spprof/_ext/platform/linux.c.backup  # US3: repository cleanup
```

**Structure Decision**: Minimal changes to existing structure. No new files created; only modifications to existing platform code and Python API.

## Complexity Tracking

> No violations to justify - changes are straightforward bug fixes and cleanups.

| Change | Justification |
|--------|---------------|
| Copy Linux signal-drain pattern to macOS | Proven pattern already working on Linux |
| Calculate overhead from existing metrics | Simple math: `(samples * handler_time) / duration` |
