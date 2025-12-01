# Tasks: Darwin Mach-Based Sampler

**Input**: Design documents from `/specs/003-darwin-mach-sampler/`  
**Prerequisites**: plan.md, spec.md, research.md, data-model.md, contracts/mach-sampler-api.md, quickstart.md

**Tests**: Integration tests included for Python-level validation. No TDD requested.

**Organization**: Tasks grouped by user story to enable independent implementation and testing.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (US1, US2, US3, US4, US5)
- Exact file paths included in descriptions

## Path Conventions

- **Source**: `src/spprof/_ext/platform/` for Mach sampler implementation
- **Tests**: `tests/` for Python integration tests

---

## Phase 1: Setup

**Purpose**: Create file structure and header definitions for the Mach sampler

- [X] T001 [P] Create darwin_mach.h with public API declarations in src/spprof/_ext/platform/darwin_mach.h
- [X] T002 [P] Create darwin_mach.c skeleton with includes and static globals in src/spprof/_ext/platform/darwin_mach.c
- [X] T003 Add darwin_mach.c to build system in setup.py (sources list for Darwin)

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Core infrastructure that MUST be complete before ANY user story can be implemented

**⚠️ CRITICAL**: No user story work can begin until this phase is complete

- [X] T004 Define ThreadEntry struct with mach_port, pthread, thread_id, stack bounds, is_valid in src/spprof/_ext/platform/darwin_mach.c
- [X] T005 Define ThreadRegistry struct with lock, entries array, count, capacity, sampler_thread in src/spprof/_ext/platform/darwin_mach.c
- [X] T006 Implement registry_init() to allocate initial capacity and initialize mutex in src/spprof/_ext/platform/darwin_mach.c
- [X] T007 Implement registry_cleanup() to free entries and destroy mutex in src/spprof/_ext/platform/darwin_mach.c
- [X] T008 Define MachSamplerConfig struct with interval_ns, native_unwinding, max_stack_depth in src/spprof/_ext/platform/darwin_mach.c
- [X] T009 Define MachSamplerStats struct with samples_captured, samples_dropped, threads_sampled, etc. in src/spprof/_ext/platform/darwin_mach.c
- [X] T010 Define MachSamplerState struct combining config, timing, threading, registry, ringbuffer, stats in src/spprof/_ext/platform/darwin_mach.c
- [X] T011 Define RegisterState struct with pc, sp, fp, lr for unified register access in src/spprof/_ext/platform/darwin_mach.c
- [X] T012 Define CapturedFrame and CapturedStack structs for stack walk results in src/spprof/_ext/platform/darwin_mach.c
- [X] T013 Implement ns_to_mach() and mach_to_ns() timing conversion functions in src/spprof/_ext/platform/darwin_mach.c

**Checkpoint**: Foundation ready - all data structures defined, user story implementation can begin

---

## Phase 3: User Story 1 - Profile Python Applications on macOS (Priority: P1) 🎯 MVP

**Goal**: Enable accurate per-thread sampling on macOS using Mach APIs, capturing samples from ALL active threads

**Independent Test**: Run multi-threaded Python app, verify samples from all threads (not just main)

### Implementation for User Story 1

- [X] T014 [US1] Implement registry_add() to add thread with mach_port, pthread, thread_id, stack bounds in src/spprof/_ext/platform/darwin_mach.c
- [X] T015 [US1] Implement registry_remove() to mark thread as invalid on THREAD_TERMINATE in src/spprof/_ext/platform/darwin_mach.c
- [X] T016 [US1] Implement registry_snapshot() to copy thread entries under lock for iteration in src/spprof/_ext/platform/darwin_mach.c
- [X] T017 [US1] Implement registry_compact() to remove invalid entries periodically in src/spprof/_ext/platform/darwin_mach.c
- [X] T018 [US1] Implement introspection_hook() callback for THREAD_START and THREAD_TERMINATE events in src/spprof/_ext/platform/darwin_mach.c
- [X] T019 [US1] Implement mach_sampler_init() to init registry and install pthread_introspection_hook in src/spprof/_ext/platform/darwin_mach.c
- [X] T020 [US1] Implement mach_sampler_cleanup() to stop sampler, remove hook, cleanup registry in src/spprof/_ext/platform/darwin_mach.c
- [X] T021 [US1] Implement validate_frame_pointer() to check bounds and alignment in src/spprof/_ext/platform/darwin_mach.c
- [X] T022 [US1] Implement walk_stack() to traverse frame pointer chain and populate CapturedStack in src/spprof/_ext/platform/darwin_mach.c
- [X] T023 [US1] Implement sample_thread() with suspend, get_state, walk_stack, resume cycle in src/spprof/_ext/platform/darwin_mach.c
- [X] T024 [US1] Implement write_sample_to_ringbuffer() to convert CapturedStack to RawSample in src/spprof/_ext/platform/darwin_mach.c
- [X] T025 [US1] Implement sampler_thread_func() main loop with mach_wait_until timing in src/spprof/_ext/platform/darwin_mach.c
- [X] T026 [US1] Implement mach_sampler_start() to create and start sampler thread in src/spprof/_ext/platform/darwin_mach.c
- [X] T027 [US1] Implement mach_sampler_stop() to signal stop and join sampler thread in src/spprof/_ext/platform/darwin_mach.c
- [X] T028 [US1] Update platform_init() to call mach_sampler_init() in src/spprof/_ext/platform/darwin.c
- [X] T029 [US1] Update platform_cleanup() to call mach_sampler_cleanup() in src/spprof/_ext/platform/darwin.c
- [X] T030 [US1] Update platform_timer_create() to call mach_sampler_start() in src/spprof/_ext/platform/darwin.c
- [X] T031 [US1] Update platform_timer_destroy() to call mach_sampler_stop() in src/spprof/_ext/platform/darwin.c
- [X] T032 [US1] Create test_darwin_mach.py with test_multithread_sampling() in tests/test_darwin_mach.py
- [X] T033 [US1] Add test_sampling_rate_accuracy() to verify 95%+ rate accuracy in tests/test_darwin_mach.py

**Checkpoint**: User Story 1 complete - profiler captures samples from all threads on macOS

---

## Phase 4: User Story 2 - Profile Mixed Python/Native Code (Priority: P2)

**Goal**: Capture both Python and native C/C++ stack frames for mixed-mode profiling

**Independent Test**: Profile Python code calling NumPy, verify both Python and native frames appear

### Implementation for User Story 2

- [X] T034 [US2] Implement mach_sampler_set_native_unwinding() to enable/disable native frame capture in src/spprof/_ext/platform/darwin_mach.c
- [X] T035 [US2] Implement mach_sampler_get_native_unwinding() to check native unwinding state in src/spprof/_ext/platform/darwin_mach.c
- [X] T036 [US2] Update write_sample_to_ringbuffer() to store native IPs in frames[] array when enabled in src/spprof/_ext/platform/darwin_mach.c
- [X] T037 [US2] Update signal_handler_set_native() to call mach_sampler_set_native_unwinding() in src/spprof/_ext/signal_handler.c
- [X] T038 [US2] Add test_mixed_mode_profiling() to verify Python+native frames captured in tests/test_darwin_mach.py

**Checkpoint**: User Story 2 complete - mixed-mode profiling works on macOS

---

## Phase 5: User Story 3 - Minimal Performance Impact (Priority: P2)

**Goal**: Ensure profiling overhead is <5% and suspension time <100μs

**Independent Test**: Run CPU-bound workload with/without profiling, measure overhead

### Implementation for User Story 3

- [X] T039 [US3] Add suspend timing measurement in sample_thread() using mach_absolute_time in src/spprof/_ext/platform/darwin_mach.c
- [X] T040 [US3] Implement mach_sampler_get_stats() for samples_captured, samples_dropped, threads_sampled in src/spprof/_ext/platform/darwin_mach.c
- [X] T041 [US3] Implement mach_sampler_get_extended_stats() including suspend_time_ns, max_suspend_ns in src/spprof/_ext/platform/darwin_mach.c
- [X] T042 [US3] Update platform_get_stats() to call mach_sampler_get_stats() in src/spprof/_ext/platform/darwin.c
- [X] T043 [US3] Add test_suspension_time() to verify <100μs per-sample suspension in tests/test_darwin_mach.py
- [X] T044 [US3] Add test_profiling_overhead() to verify <5% slowdown at 100Hz in tests/test_darwin_mach.py

**Checkpoint**: User Story 3 complete - performance requirements validated

---

## Phase 6: User Story 4 - Reliable Thread Discovery (Priority: P3)

**Goal**: Automatically discover all threads including dynamically spawned and GCD threads

**Independent Test**: Spawn threads during profiling, verify they are sampled

### Implementation for User Story 4

- [X] T045 [US4] Handle KERN_TERMINATED and KERN_INVALID_ARGUMENT in sample_thread() gracefully in src/spprof/_ext/platform/darwin_mach.c
- [X] T046 [US4] Add threads_skipped counter increment for terminated/invalid threads in src/spprof/_ext/platform/darwin_mach.c
- [X] T047 [US4] Implement mach_sampler_thread_count() to return current registry count in src/spprof/_ext/platform/darwin_mach.c
- [X] T048 [US4] Call registry_compact() periodically in sampler loop to cleanup invalid entries in src/spprof/_ext/platform/darwin_mach.c
- [X] T049 [US4] Add test_dynamic_thread_discovery() to verify new threads are sampled in tests/test_darwin_mach.py
- [X] T050 [US4] Add test_thread_termination_handling() to verify graceful handling in tests/test_darwin_mach.py
- [X] T051 [US4] Handle already-suspended threads gracefully in sample_thread() (skip if suspend fails) in src/spprof/_ext/platform/darwin_mach.c
- [X] T052 [US4] Add test_blocked_thread_sampling() to verify threads blocked on I/O are sampled in tests/test_darwin_mach.py

**Checkpoint**: User Story 4 complete - thread discovery handles all lifecycle scenarios

---

## Phase 7: User Story 5 - Architecture Support (Priority: P3)

**Goal**: Support both x86_64 (Intel) and arm64 (Apple Silicon) architectures

**Independent Test**: Build and run on both Intel and Apple Silicon Macs

**Note**: Tasks T053-T058 can be executed in parallel with Phase 3 (US1) since they have no dependencies on sampling implementation.

### Implementation for User Story 5

- [X] T053 [P] [US5] Implement get_register_state_x86_64() using x86_THREAD_STATE64 in src/spprof/_ext/platform/darwin_mach.c
- [X] T054 [P] [US5] Implement get_register_state_arm64() using ARM_THREAD_STATE64 with accessor macros in src/spprof/_ext/platform/darwin_mach.c
- [X] T055 [US5] Implement get_register_state() dispatcher with #if defined(__x86_64__) / defined(__arm64__) in src/spprof/_ext/platform/darwin_mach.c
- [X] T056 [US5] Add compile-time check for unsupported architectures with #error in src/spprof/_ext/platform/darwin_mach.c
- [X] T057 [US5] Add test_architecture_detection() to verify correct architecture used in tests/test_darwin_mach.py
- [X] T058 [US5] Add Darwin platform skip marker for non-Darwin CI runs in tests/test_darwin_mach.py

**Checkpoint**: User Story 5 complete - profiler works on both Intel and Apple Silicon

---

## Phase 8: Polish & Cross-Cutting Concerns

**Purpose**: Final integration, documentation, verification, and cleanup

- [X] T059 [P] Add comprehensive docstrings to all public functions in src/spprof/_ext/platform/darwin_mach.h
- [X] T060 [P] Add SPPROF_DEBUG logging statements to key functions in src/spprof/_ext/platform/darwin_mach.c
- [X] T061 Remove or deprecate setitimer code path in darwin.c (keep as fallback comment) in src/spprof/_ext/platform/darwin.c
- [X] T062 Verify framewalker.c compatibility with Mach-based sampler (external thread walking) in src/spprof/_ext/framewalker.c
- [X] T063 Add test_no_privilege_required() verifying no entitlement failures on hardened runtime in tests/test_darwin_mach.py
- [X] T064 Add interval validation (minimum 1ms) in mach_sampler_start() in src/spprof/_ext/platform/darwin_mach.c
- [X] T065 Update existing test_threading.py to include Darwin multi-thread verification in tests/test_threading.py
- [X] T066 Update test_platform.py with Darwin-specific test cases in tests/test_platform.py
- [X] T067 Run full test suite on macOS and fix any regressions
- [X] T068 Validate implementation against quickstart.md scenarios

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies - can start immediately
- **Foundational (Phase 2)**: Depends on Setup completion - BLOCKS all user stories
- **User Stories (Phase 3-7)**: All depend on Foundational phase completion
  - US1 (P1): Can start immediately after Foundational
  - US2 (P2): Can start after Foundational, benefits from US1 completion
  - US3 (P2): Can start after Foundational, benefits from US1 completion
  - US4 (P3): Can start after Foundational, benefits from US1 completion
  - US5 (P3): Can start after Foundational, runs in parallel with US1
- **Polish (Phase 8)**: Depends on all user stories being complete

### User Story Dependencies

| Story | Depends On | Can Parallel With | Notes |
|-------|-----------|-------------------|-------|
| US1 (P1) | Foundational | US5 | Core MVP functionality |
| US2 (P2) | Foundational, US1 (ring buffer integration) | US3, US4 | Requires write_sample_to_ringbuffer() |
| US3 (P2) | Foundational, US1 (stats collection) | US2, US4 | Requires sample_thread() |
| US4 (P3) | Foundational | US1, US5 | Edge case handling |
| US5 (P3) | Foundational | US1, US4 | **Can start during Phase 3** - architecture code is independent |

### Within Each User Story

- Registry operations before sampler thread
- Sampler core before statistics
- Platform integration after core implementation
- Tests after implementation

### Parallel Opportunities

- T001 & T002: Header and source skeleton in parallel
- T053 & T054: x86_64 and arm64 register extraction in parallel
- Phase 7 (US5) can start during Phase 3 (US1) - architecture code has no dependencies on sampling
- All test tasks marked [P] within a phase can run in parallel
- US5 architecture support can proceed in parallel with US1 core implementation

---

## Parallel Example: Phase 2 Foundational

```bash
# These can run in parallel (different sections of same file, no dependencies):
Task T004: "Define ThreadEntry struct..."
Task T008: "Define MachSamplerConfig struct..."
Task T011: "Define RegisterState struct..."
```

## Parallel Example: User Story 1 + User Story 5

```bash
# Registry operations (sequential - build on each other):
T014 → T015 → T016 → T017 → T018 → T019 → T020

# Stack walking (after registry, can parallel with sampler):
T021 → T022

# Architecture support (can run in parallel with US1 - no dependencies):
T053, T054 can run in parallel with T014-T027

# Sampler operations (after registry and stack walking):
T023 → T024 → T025 → T026 → T027

# Platform integration (after mach_sampler_* complete):
T028, T029, T030, T031 can run in parallel

# Tests (after platform integration):
T032, T033 can run in parallel
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1: Setup
2. Complete Phase 2: Foundational
3. Complete Phase 3: User Story 1 (core sampling)
4. **STOP and VALIDATE**: Run test_multithread_sampling, test_sampling_rate_accuracy
5. Deploy/demo if ready - profiler now works for basic multi-thread Python profiling

### Incremental Delivery

1. Setup + Foundational → Foundation ready
2. Add User Story 1 → Test independently → **MVP: Basic profiling works!**
3. Add User Story 5 → Test independently → Both architectures supported
4. Add User Story 2 → Test independently → Mixed-mode profiling
5. Add User Story 3 → Test independently → Performance validated
6. Add User Story 4 → Test independently → Robust thread handling
7. Polish → Full feature complete

### Recommended Order

Given the dependencies and priorities:

1. **Phase 1-2**: Setup + Foundational (required for all)
2. **Phase 3 (US1) + Phase 7 (US5)**: Core sampling + Architecture (can parallel)
3. **Phase 4 (US2)**: Mixed-mode profiling
4. **Phase 5 (US3)**: Performance validation
5. **Phase 6 (US4)**: Thread discovery robustness
6. **Phase 8**: Polish

---

## Summary

| Phase | Tasks | Purpose | Stories |
|-------|-------|---------|---------|
| 1 | T001-T003 | Setup | - |
| 2 | T004-T013 | Foundational | - |
| 3 | T014-T033 | Core Sampling | US1 (P1) 🎯 MVP |
| 4 | T034-T038 | Mixed-Mode | US2 (P2) |
| 5 | T039-T044 | Performance | US3 (P2) |
| 6 | T045-T052 | Thread Discovery + Edge Cases | US4 (P3) |
| 7 | T053-T058 | Architecture | US5 (P3) |
| 8 | T059-T068 | Polish + Verification | - |

**Total Tasks**: 68  
**MVP Tasks**: 33 (Phases 1-3)  
**Parallel Opportunities**: 14 tasks marked [P]  
**New Tasks Added**: T051-T052 (edge cases), T062-T064 (verification)

---

## Notes

- [P] tasks = different files or independent sections, no blocking dependencies
- [Story] label maps task to specific user story for traceability
- Each user story is independently completable and testable after Foundational
- Verify tests pass before moving to next phase
- Commit after each task or logical group
- Stop at any checkpoint to validate story independently

