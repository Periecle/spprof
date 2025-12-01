# Implementation Plan: Linux timer_create Robustness Improvements

**Branch**: `001-linux-create-timer` | **Date**: 2025-12-01 | **Spec**: [spec.md](./spec.md)
**Input**: Feature specification from `/specs/001-linux-create-timer/spec.md`

## Summary

Improve the robustness of the existing Linux `timer_create()` profiler implementation by:
- Removing the fixed 256-thread limit with dynamic thread tracking
- Implementing proper timer overrun handling and statistics
- Eliminating race conditions during timer cleanup
- Using efficient O(1) data structures for thread lookup
- Adding pause/resume capabilities without timer recreation

## Technical Context

**Language/Version**: C11 (GNU extensions), Python 3.9-3.14  
**Primary Dependencies**: Linux kernel timer API (`timer_create`, `timer_settime`, `timer_delete`), pthreads, atomic operations  
**Storage**: In-memory data structures only (no persistent storage)  
**Testing**: pytest, valgrind for memory leak detection, stress tests  
**Target Platform**: Linux (kernel 2.6+)  
**Project Type**: Single project - C extension module  
**Performance Goals**: Thread registration/lookup in O(1), shutdown < 100ms  
**Constraints**: Must remain async-signal-safe in signal handler, no GIL acquisition in timer paths  
**Scale/Scope**: Support 500+ threads minimum, 1000+ concurrent threads target, handle rapid thread churn

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

The project constitution is a template without specific gates defined. Following general engineering best practices:

| Principle | Status | Notes |
|-----------|--------|-------|
| Test-First | ✅ PASS | Existing test suite will be extended |
| Backward Compatibility | ✅ PASS | `platform.h` API preserved |
| Simplicity | ✅ PASS | Minimal changes to existing architecture |
| Signal Safety | ✅ PASS | No changes to signal handler hot path |

## Project Structure

### Documentation (this feature)

```text
specs/001-linux-create-timer/
├── plan.md              # This file
├── research.md          # Phase 0 output - timer API deep dive
├── data-model.md        # Phase 1 output - thread registry design
├── quickstart.md        # Phase 1 output - development setup
├── contracts/           # Phase 1 output - internal C API contracts
│   └── thread-registry-api.md
└── tasks.md             # Phase 2 output (created by /speckit.tasks)
```

### Source Code (repository root)

```text
src/spprof/_ext/
├── platform/
│   ├── linux.c          # PRIMARY: Enhanced timer_create implementation
│   ├── darwin.c         # UNCHANGED: macOS setitimer (out of scope)
│   ├── windows.c        # UNCHANGED: Windows timer queue (out of scope)
│   └── platform.h       # MINOR: May add pause/resume declarations
├── signal_handler.c     # MINOR: Coherent updates for statistics
├── signal_handler.h     # MINOR: May expose additional stats
└── ...                  # Other files unchanged

tests/
├── test_platform.py     # EXTENDED: Linux-specific robustness tests
└── test_threading.py    # EXTENDED: Thread limit and churn tests
```

**Structure Decision**: Single project structure with changes isolated to `platform/linux.c` and related files. No new directories required.

## Complexity Tracking

No complexity violations identified. The implementation uses:
- Standard C data structures (hash table for thread registry)
- Existing Linux kernel APIs (`timer_create`, `timer_delete`)
- Existing platform abstraction layer

| Potential Complexity | Justification | Alternative Rejected |
|---------------------|---------------|---------------------|
| Hash table for thread lookup | O(1) lookup vs O(n) array scan | Array scan too slow for 500+ threads |
| RW lock for registry | Concurrent reads during profiling | Single mutex too contended |

## Phase 0: Research Summary

See [research.md](./research.md) for detailed findings on:
1. Dynamic memory allocation strategies for thread registry
2. Timer overrun handling via `timer_getoverrun()`
3. Race-free shutdown patterns for POSIX timers
4. Hash table implementation options (uthash vs custom)
5. Pause/resume via `timer_settime()` with zero interval

## Phase 1: Design Summary

See [data-model.md](./data-model.md) for:
- `ThreadTimerEntry` structure design
- `ThreadTimerRegistry` hash table design
- Thread-safe access patterns

See [contracts/thread-registry-api.md](./contracts/thread-registry-api.md) for:
- Internal C API function signatures
- Error handling patterns
- Thread safety guarantees

## Key Implementation Decisions

| Decision | Rationale |
|----------|-----------|
| Use `uthash` for thread registry | Header-only, well-tested, no external dependencies |
| RWLock for registry access | Reads (stat queries) shouldn't block writes (thread registration) |
| Use uthash dynamic growth | uthash handles resizing automatically; no manual capacity management needed |
| Aggregate overruns per thread | More useful than single global counter |
| Signal blocking during cleanup | Prevents race between timer deletion and signal delivery |

## Risk Assessment

| Risk | Likelihood | Impact | Mitigation |
|------|------------|--------|------------|
| Memory leak on thread exit | Medium | High | Test with valgrind in CI |
| Race condition in registry | Medium | High | Use RWLock + careful ordering |
| Breaking existing behavior | Low | High | Preserve all platform.h signatures |
| Performance regression | Low | Medium | Benchmark before/after |
