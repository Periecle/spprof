# Tasks: Linux timer_create Robustness Improvements

**Input**: Design documents from `/specs/001-linux-create-timer/`
**Prerequisites**: plan.md, spec.md, research.md, data-model.md, contracts/thread-registry-api.md

**Tests**: Included as validation tests per user story (stress tests, memory leak tests required by spec).

**Organization**: Tasks grouped by user story for independent implementation and testing.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (US1, US2, US3, US4)
- Include exact file paths in descriptions

## Path Conventions

- **Source**: `src/spprof/_ext/platform/linux.c` (primary)
- **Header**: `src/spprof/_ext/platform/platform.h`
- **Tests**: `tests/test_platform.py`, `tests/test_threading.py`

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Project initialization and dependency setup

- [X] T001 Add uthash header file to `src/spprof/_ext/uthash.h` (download from https://github.com/troydhanson/uthash)
- [X] T002 [P] Verify build works with uthash included via `pip install -e .`
- [X] T003 [P] Create backup of current `src/spprof/_ext/platform/linux.c` for reference

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Core infrastructure that MUST be complete before ANY user story can be implemented

**⚠️ CRITICAL**: No user story work can begin until this phase is complete

- [X] T004 Define `ThreadTimerEntry` struct in `src/spprof/_ext/platform/linux.c` with uthash integration (tid, timer_id, overruns, active, UT_hash_handle hh)
- [X] T005 Add global registry pointer and RWLock in `src/spprof/_ext/platform/linux.c` (g_thread_registry, g_registry_lock)
- [X] T006 [P] Add atomic statistics counters in `src/spprof/_ext/platform/linux.c` (g_total_overruns, g_timer_create_failures)
- [X] T007 [P] Add pause state variables in `src/spprof/_ext/platform/linux.c` (g_paused, g_saved_interval_ns)
- [X] T008 Implement `registry_init()` function in `src/spprof/_ext/platform/linux.c` (initialize RWLock, set registry to NULL)
- [X] T009 Implement `registry_cleanup()` function in `src/spprof/_ext/platform/linux.c` (iterate and free all entries, delete timers)
- [X] T010 Update `platform_init()` to call `registry_init()` in `src/spprof/_ext/platform/linux.c`
- [X] T011 Update `platform_cleanup()` to call `registry_cleanup()` in `src/spprof/_ext/platform/linux.c`

**Checkpoint**: Foundation ready - registry infrastructure in place

---

## Phase 3: User Story 1 - Reliable Multi-Threaded Profiling (Priority: P1) 🎯 MVP

**Goal**: Remove the fixed 256-thread limit by implementing dynamic thread tracking with uthash hash table

**Independent Test**: Profile a Python application that spawns 300+ worker threads and verify all threads are registered and sampled.

### Implementation for User Story 1

- [X] T012 [US1] Implement `registry_add_thread(pid_t tid, timer_t timer_id)` in `src/spprof/_ext/platform/linux.c`
- [X] T013 [US1] Implement `registry_find_thread(pid_t tid)` in `src/spprof/_ext/platform/linux.c`
- [X] T014 [US1] Implement `registry_remove_thread(pid_t tid)` in `src/spprof/_ext/platform/linux.c`
- [X] T015 [US1] Implement `registry_count()` in `src/spprof/_ext/platform/linux.c`
- [X] T016 [US1] Refactor `platform_register_thread()` to use registry instead of fixed array in `src/spprof/_ext/platform/linux.c`
- [X] T017 [US1] Refactor `platform_unregister_thread()` to use registry in `src/spprof/_ext/platform/linux.c`
- [X] T018 [US1] Remove old `MAX_TRACKED_THREADS` constant and `g_thread_timers` array from `src/spprof/_ext/platform/linux.c`
- [X] T019 [US1] Remove old `g_thread_count` variable from `src/spprof/_ext/platform/linux.c`
- [X] T020 [US1] Add error handling for `timer_create` failures with retry logic (EAGAIN) in `src/spprof/_ext/platform/linux.c`
- [X] T021 [US1] Add failure counter increment on timer creation failure in `src/spprof/_ext/platform/linux.c`
- [X] T022 [US1] Add test `test_many_threads()` for 300+ threads in `tests/test_threading.py`
- [X] T023 [US1] Add test `test_rapid_thread_churn()` for thread pool patterns in `tests/test_threading.py`

**Checkpoint**: User Story 1 complete - applications with 500+ threads can be profiled

---

## Phase 4: User Story 2 - Accurate Sampling Under High Load (Priority: P2)

**Goal**: Track and expose timer overrun counts via statistics API for profiling accuracy assessment

**Independent Test**: Profile a CPU-intensive application, query statistics, and verify overrun count is reported.

### Implementation for User Story 2

- [X] T024 [US2] Add overrun tracking to global atomic counter in `src/spprof/_ext/platform/linux.c`
- [X] T025 [US2] Implement `registry_get_total_overruns()` in `src/spprof/_ext/platform/linux.c`
- [X] T026 [US2] Implement `registry_add_overruns(uint64_t count)` in `src/spprof/_ext/platform/linux.c`
- [X] T027 [US2] Update `platform_timer_destroy()` to capture final overrun count via `timer_getoverrun()` before deletion in `src/spprof/_ext/platform/linux.c`
- [X] T028 [US2] Implement `registry_get_create_failures()` in `src/spprof/_ext/platform/linux.c`
- [X] T029 [US2] Update `platform_get_stats()` to include timer_overruns in `src/spprof/_ext/platform/linux.c`
- [X] T030 [US2] Add `platform_get_extended_stats()` declaration to `src/spprof/_ext/platform/platform.h`
- [X] T031 [US2] Implement `platform_get_extended_stats()` in `src/spprof/_ext/platform/linux.c`
- [X] T032 [US2] Add test `test_timer_overrun_stats()` to verify overrun reporting in `tests/test_platform.py`

**Checkpoint**: User Story 2 complete - timer overruns accurately reported in statistics

---

## Phase 5: User Story 3 - Clean Profiler Shutdown (Priority: P2)

**Goal**: Eliminate race conditions during timer cleanup using signal blocking pattern

**Independent Test**: Start and stop the profiler 1000 times in a loop while threads are created/destroyed, verify no crashes (per SC-003).

### Implementation for User Story 3

- [X] T033 [US3] Refactor `platform_timer_destroy()` to block SIGPROF before cleanup in `src/spprof/_ext/platform/linux.c`
- [X] T034 [US3] Add `sigtimedwait()` loop to drain pending signals in `platform_timer_destroy()` in `src/spprof/_ext/platform/linux.c`
- [X] T035 [US3] Update `registry_cleanup()` to block signals during iteration in `src/spprof/_ext/platform/linux.c`
- [X] T036 [US3] Remove hacky `nanosleep()` workaround from `platform_timer_destroy()` in `src/spprof/_ext/platform/linux.c`
- [X] T037 [US3] Ensure signal mask is restored after cleanup in `src/spprof/_ext/platform/linux.c`
- [X] T038 [US3] Add test `test_start_stop_stress()` with 1000 start/stop cycles in `tests/test_platform.py` (per SC-003)
- [X] T039 [US3] Add test `test_concurrent_thread_registration()` with concurrent add/remove in `tests/test_threading.py`

**Checkpoint**: User Story 3 complete - profiler starts and stops cleanly without races

---

## Phase 6: User Story 4 - Pause and Resume Profiling (Priority: P3)

**Goal**: Support pausing and resuming profiling without destroying timers using zero-interval disarm

**Independent Test**: Pause profiling, execute code, resume profiling, verify no samples captured during pause.

### Implementation for User Story 4

- [X] T040 [US4] Add `platform_timer_pause()` declaration to `src/spprof/_ext/platform/platform.h`
- [X] T041 [US4] Add `platform_timer_resume()` declaration to `src/spprof/_ext/platform/platform.h`
- [X] T042 [US4] Implement `platform_timer_pause()` in `src/spprof/_ext/platform/linux.c` (save interval, set zero interval)
- [X] T043 [US4] Implement `registry_pause_all()` helper in `src/spprof/_ext/platform/linux.c`
- [X] T044 [US4] Implement `platform_timer_resume()` in `src/spprof/_ext/platform/linux.c` (restore saved interval)
- [X] T045 [US4] Implement `registry_resume_all(uint64_t interval_ns)` helper in `src/spprof/_ext/platform/linux.c`
- [X] T046 [US4] Update active flag tracking during pause/resume in `src/spprof/_ext/platform/linux.c`
- [X] T047 [US4] Add stub implementations for macOS in `src/spprof/_ext/platform/darwin.c` (return 0, no-op for API compatibility)
- [X] T048 [US4] Add stub implementations for Windows in `src/spprof/_ext/platform/windows.c` (return 0, no-op for API compatibility)
- [X] T049 [US4] Add test `test_pause_resume_basic()` in `tests/test_platform.py`
- [X] T050 [US4] Add test `test_pause_no_samples()` verifying no samples during pause in `tests/test_platform.py`

**Checkpoint**: User Story 4 complete - pause/resume works without timer recreation

---

## Phase 7: Polish & Cross-Cutting Concerns

**Purpose**: Final validation, documentation, and cleanup

- [X] T051 [P] Run full test suite: `pytest tests/ -v`
- [ ] T052 [P] Run valgrind memory check: `valgrind --leak-check=full python -m pytest tests/test_platform.py -v -k linux`
- [X] T053 [P] Verify backward compatibility: existing tests pass without modification
- [X] T054 Update `docs/ARCHITECTURE.md` with thread registry documentation
- [X] T055 [P] Add inline code comments for new registry functions in `src/spprof/_ext/platform/linux.c`
- [X] T056 Run quickstart.md validation steps
- [X] T057 [P] Add test `test_shutdown_timing()` to verify profiler shutdown completes within 100ms in `tests/test_platform.py` (per SC-005)
- [X] T058 [P] Add test `test_thread_exit_during_timer()` for edge case: thread exits while timer firing in `tests/test_threading.py`
- [X] T059 [P] Add test `test_sigprof_blocked()` for edge case: SIGPROF blocked by application in `tests/test_platform.py`

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies - can start immediately
- **Foundational (Phase 2)**: Depends on Setup completion - BLOCKS all user stories
- **User Stories (Phase 3-6)**: All depend on Foundational phase completion
  - US1, US2, US3 can proceed in parallel after Foundational
  - US4 can proceed after Foundational (independent of other stories)
- **Polish (Phase 7)**: Depends on desired user stories being complete

### User Story Dependencies

- **User Story 1 (P1)**: Can start after Foundational - No dependencies on other stories
- **User Story 2 (P2)**: Can start after Foundational - Uses registry from US1 but can be implemented independently
- **User Story 3 (P2)**: Can start after Foundational - Uses registry from US1 but independent cleanup logic
- **User Story 4 (P3)**: Can start after Foundational - Uses registry but adds new functionality

### Within Each User Story

- Registry functions before platform functions that use them
- Core implementation before error handling refinements
- Implementation before tests

### Parallel Opportunities

**Phase 1 (Setup)**:
- T002 and T003 can run in parallel

**Phase 2 (Foundational)**:
- T006 and T007 can run in parallel (different variables)
- T004, T005 must complete before T008, T009

**Phase 3 (US1)** - After T015 completes:
- T022 and T023 (tests) can run in parallel

**Phase 7 (Polish)**:
- T051, T052, T053, T055 can all run in parallel

---

## Parallel Example: Phase 2 Foundational

```bash
# Run in parallel - independent global variables:
Task T006: "Add atomic statistics counters in src/spprof/_ext/platform/linux.c"
Task T007: "Add pause state variables in src/spprof/_ext/platform/linux.c"
```

## Parallel Example: User Story 1 Tests

```bash
# After US1 implementation complete, run tests in parallel:
Task T022: "Add test test_many_threads() for 300+ threads in tests/test_threading.py"
Task T023: "Add test test_rapid_thread_churn() for thread pool patterns in tests/test_threading.py"
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1: Setup (T001-T003)
2. Complete Phase 2: Foundational (T004-T011)
3. Complete Phase 3: User Story 1 (T012-T023)
4. **STOP and VALIDATE**: Run `test_many_threads()` with 300+ threads
5. This delivers the core value: removing the 256-thread limit

### Incremental Delivery

1. Setup + Foundational → Registry infrastructure ready
2. Add User Story 1 → Test with many threads → **MVP Complete!**
3. Add User Story 2 → Test overrun statistics → Improved observability
4. Add User Story 3 → Test start/stop cycles → Production stability
5. Add User Story 4 → Test pause/resume → Advanced profiling

### Single Developer Sequence

```
Phase 1 → Phase 2 → Phase 3 (MVP) → Phase 4 → Phase 5 → Phase 6 → Phase 7
```

---

## Summary

| Phase | Tasks | Parallel Opportunities |
|-------|-------|------------------------|
| Setup | 3 | 2 |
| Foundational | 8 | 2 |
| US1 (P1) | 12 | 2 |
| US2 (P2) | 9 | 0 |
| US3 (P2) | 7 | 0 |
| US4 (P3) | 11 | 0 |
| Polish | 9 | 7 |
| **Total** | **59** | **13** |

### Task Count by User Story

- **US1**: 12 tasks (MVP - remove thread limit)
- **US2**: 9 tasks (overrun tracking)
- **US3**: 7 tasks (race-free shutdown)
- **US4**: 11 tasks (pause/resume)

### MVP Scope

**Minimum Viable Product**: Complete Phases 1-3 (User Story 1)
- 23 tasks total for MVP
- Delivers: Dynamic thread tracking with no artificial limits
- Validates: 300+ threads can be profiled

---

## Notes

- [P] tasks = different files or independent changes, no dependencies
- [Story] label maps task to specific user story for traceability
- Each user story is independently testable after Foundational phase
- Verify existing tests pass after each phase (backward compatibility)
- Run valgrind after each user story to catch memory leaks early
- Commit after each task or logical group

