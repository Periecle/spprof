# Implementation Plan: Darwin Mach-Based Sampler

**Branch**: `003-darwin-mach-sampler` | **Date**: 2024-12-01 | **Spec**: [spec.md](./spec.md)
**Input**: Feature specification from `/specs/003-darwin-mach-sampler/spec.md`

## Summary

Replace the current `setitimer(ITIMER_PROF)`-based profiler on macOS with a Mach kernel-based "Suspend-Walk-Resume" architecture. This eliminates the fundamental limitations of signal-based sampling (random thread targeting, signal-safety constraints) by using a dedicated sampler thread that:

1. Discovers threads via `pthread_introspection_hook_install`
2. Suspends target threads via `thread_suspend()`
3. Captures register state via `thread_get_state()`
4. Walks frame pointers directly from memory
5. Resumes threads via `thread_resume()`

This approach mirrors the Windows `SuspendThread` architecture already implemented in `platform/windows.c`, enabling code reuse and unified behavior across platforms.

## Technical Context

**Language/Version**: C (with Python C API), targeting Python 3.9-3.14  
**Primary Dependencies**: Mach kernel APIs (`<mach/mach.h>`), pthread introspection (`pthread_introspection_hook_install`)  
**Storage**: Lock-free ring buffer (existing `ringbuffer.h`)  
**Testing**: pytest for Python integration, C unit tests for low-level components  
**Target Platform**: macOS 10.15+ (Catalina and later), supporting x86_64 and arm64  
**Project Type**: Single library (Python C extension module)  
**Performance Goals**: <5% overhead at 100Hz, <100μs per-sample suspension time  
**Constraints**: Must work without elevated privileges or special entitlements  
**Scale/Scope**: Single-process profiling, all threads within process

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

The project constitution is a template without specific gates defined. The following standard quality gates apply:

| Gate | Status | Notes |
|------|--------|-------|
| Fits existing architecture | ✅ PASS | Follows platform abstraction pattern in `platform/` directory |
| No new dependencies | ✅ PASS | Uses only macOS system APIs (Mach kernel) |
| Maintains platform API | ✅ PASS | Implements existing `platform.h` interface |
| Async-signal-safe not required | ✅ PASS | Mach approach eliminates signal handler constraints |
| Backward compatible | ✅ PASS | Same Python API, different internal implementation |

## Project Structure

### Documentation (this feature)

```text
specs/003-darwin-mach-sampler/
├── plan.md              # This file
├── research.md          # Phase 0: Mach API research
├── data-model.md        # Phase 1: Thread registry data structures
├── quickstart.md        # Phase 1: Implementation guide
├── contracts/           # Phase 1: Internal C API contracts
│   └── mach-sampler-api.md
└── tasks.md             # Phase 2 output (via /speckit.tasks)
```

### Source Code (repository root)

```text
src/spprof/_ext/
├── platform/
│   ├── darwin.c         # MODIFIED: Replace setitimer with Mach sampler
│   ├── darwin_mach.c    # NEW: Mach-specific sampler implementation
│   ├── darwin_mach.h    # NEW: Mach sampler internal interface
│   └── platform.h       # UNCHANGED: Platform abstraction (already supports this)
├── framewalker.c        # MINOR: May need adjustment for external thread walking
├── ringbuffer.h         # UNCHANGED: Reuse existing infrastructure
└── signal_handler.c     # UNCHANGED: Not used on Darwin with Mach approach

tests/
├── test_darwin_mach.py  # NEW: Darwin Mach sampler tests
├── test_threading.py    # MODIFIED: Add multi-thread sampling verification
└── test_platform.py     # MODIFIED: Darwin-specific test cases
```

**Structure Decision**: Follows existing platform abstraction pattern. New Mach-specific code isolated in `darwin_mach.c/h` to keep `darwin.c` as a thin dispatcher that can switch between setitimer (fallback) and Mach (preferred) implementations.

## Complexity Tracking

No constitution violations requiring justification. The design follows existing patterns:

- Platform-specific code in `platform/` directory
- Reuses existing `RingBuffer` and `RawSample` structures
- Implements existing `platform.h` interface
- No new external dependencies

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────────┐
│                        Python Process                                │
├─────────────────────────────────────────────────────────────────────┤
│                                                                      │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────┐          │
│  │   Thread 1   │    │   Thread 2   │    │   Thread N   │          │
│  │  (Python)    │    │  (Python)    │    │  (GCD/etc)   │          │
│  └──────────────┘    └──────────────┘    └──────────────┘          │
│         │                   │                   │                   │
│         └───────────────────┼───────────────────┘                   │
│                             │                                        │
│                    pthread_introspection                             │
│                       hook callbacks                                 │
│                             │                                        │
│                             ▼                                        │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │                    Thread Registry                           │   │
│  │  ┌─────────────────────────────────────────────────────┐    │   │
│  │  │  thread_act_t ports[]  │  Lock (pthread_mutex)      │    │   │
│  │  └─────────────────────────────────────────────────────┘    │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                             │                                        │
│                             ▼                                        │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │                    Sampler Thread                            │   │
│  │                                                              │   │
│  │   mach_wait_until() ──► for each thread:                    │   │
│  │         │                  thread_suspend()                  │   │
│  │         │                  thread_get_state()                │   │
│  │         │                  walk_frame_pointers()             │   │
│  │         │                  thread_resume()                   │   │
│  │         │                                                    │   │
│  │         └──────────────────────────────────────────────────►│   │
│  └─────────────────────────────────────────────────────────────┘   │
│                             │                                        │
│                             ▼                                        │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │                    Ring Buffer                               │   │
│  │              (RawSample - existing structure)               │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                             │                                        │
│                             ▼                                        │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │                    Resolver Thread                           │   │
│  │              (existing - unchanged)                          │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                                                                      │
└─────────────────────────────────────────────────────────────────────┘
```

## Key Design Decisions

### 1. Thread Discovery via pthread_introspection_hook

**Decision**: Use `pthread_introspection_hook_install()` to track thread lifecycle.

**Rationale**: 
- Apple-specific API that hooks into pthread creation/destruction
- Captures ALL threads including GCD worker threads
- Alternative (enumerating via `task_threads()`) requires per-sample syscall overhead

**Trade-off**: Non-standard API, but officially documented by Apple and stable.

### 2. Direct Frame Pointer Walking

**Decision**: Walk frame pointers directly via memory dereference, not libunwind.

**Rationale**:
- macOS ABI (especially arm64) guarantees frame pointers
- Direct memory access is faster than libunwind
- Thread is suspended, so memory is stable
- Validates pointers against stack bounds for safety

**Trade-off**: Won't work for code compiled with `-fomit-frame-pointer`, but this is rare on macOS.

### 3. Sampler Thread Architecture

**Decision**: Dedicated sampler thread with precise sleep (`mach_wait_until`).

**Rationale**:
- Eliminates signal-safety constraints (can malloc, log, use locks)
- Provides deterministic targeting (iterate known threads)
- Mirrors Windows architecture for code reuse
- `mach_wait_until` provides nanosecond precision

### 4. Stack Bounds Validation

**Decision**: Validate frame pointers against `pthread_get_stacksize_np()` bounds.

**Rationale**:
- Prevents sampler thread crash from dereferencing invalid pointers
- Stack bounds are available without syscall (thread-local)
- Gracefully terminates walk on invalid pointer

## Implementation Phases

### Phase 1: Thread Registry (P1 - Core)
- Implement `pthread_introspection_hook_install` callback
- Thread-safe registry of `thread_act_t` ports
- Handle thread creation/destruction races

### Phase 2: Sampler Core (P1 - Core)
- Sampler thread with `mach_wait_until` timing
- Thread suspension via `thread_suspend()`
- Register capture via `thread_get_state()` (x86_64 + arm64)
- Thread resume via `thread_resume()`

### Phase 3: Stack Walking (P1 - Core)
- Frame pointer walking for arm64 (FP → caller FP, FP+8 → return address)
- Frame pointer walking for x86_64 (RBP chain)
- Stack bounds validation
- Integration with existing `RawSample` structure

### Phase 4: Integration (P2 - Mixed Mode)
- Connect to existing ring buffer
- Python frame resolution (post-walk, with GIL)
- Native symbol resolution for mixed-mode
- Statistics collection

### Phase 5: Testing & Validation
- Multi-thread sampling distribution tests
- Performance benchmarks (overhead, suspension time)
- Architecture tests (x86_64, arm64)
- Edge case handling (thread termination during walk)
