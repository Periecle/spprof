# Tasks: Resolve TODOs, Race Conditions, and Incomplete Implementations

**Input**: Design documents from `/specs/002-resolve-todos-cleanup/`
**Prerequisites**: plan.md ✓, spec.md ✓, research.md ✓, quickstart.md ✓

**Tests**: Not explicitly requested. Validation uses existing test suite.

**Organization**: Tasks grouped by user story for independent implementation and testing.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (US1, US2, US3)
- Include exact file paths in descriptions

## Path Conventions

- **Source**: `src/spprof/` (Python API)
- **C Extension**: `src/spprof/_ext/platform/` (platform implementations)
- **Tests**: `tests/` (existing test suite)

---

## Phase 1: Setup

**Purpose**: Verify current state before making changes

- [X] T001 Verify existing test suite passes with `pytest tests/ -v`
- [X] T002 [P] Confirm `_get_stats()` returns `dropped_samples` from native by inspecting `src/spprof/_ext/module.c`

**Checkpoint**: Baseline verified - ready to implement changes

---

## Phase 2: User Story 1 - Complete Statistics API (Priority: P1) 🎯 MVP

**Goal**: Wire up dropped sample counts and overhead estimation from native to Python API

**Independent Test**: Run profiling with 1ms interval under load, verify `stats().dropped_samples` and `stats().overhead_estimate_pct` return meaningful values

### Implementation for User Story 1

- [X] T003 [US1] Update `stats()` function in `src/spprof/__init__.py` to use actual `dropped_samples` from `_native._get_stats()` (line 292-296)
- [X] T004 [US1] Implement overhead estimation calculation in `stats()` in `src/spprof/__init__.py` using formula: `(collected * 0.025) / duration_ms * 100`
- [X] T005 [US1] Update `stop()` function in `src/spprof/__init__.py` to get final `dropped_count` from native stats before returning Profile (line 240-243)
- [X] T006 [US1] Validate US1 by running quickstart.md "Test Dropped Samples" script

**Checkpoint**: User Story 1 complete - statistics API returns accurate values

---

## Phase 3: User Story 2 - Race-Free macOS Shutdown (Priority: P2)

**Goal**: Replace naive `nanosleep()` workaround with proper signal blocking and draining on macOS

**Independent Test**: Run 1000 start/stop cycles on macOS without crashes (per quickstart.md)

### Implementation for User Story 2

- [X] T007 [US2] Add SIGPROF blocking at start of `platform_timer_destroy()` in `src/spprof/_ext/platform/darwin.c` using `pthread_sigmask()`
- [X] T008 [US2] Replace `nanosleep()` with `sigtimedwait()` drain loop in `platform_timer_destroy()` in `src/spprof/_ext/platform/darwin.c`
- [X] T009 [US2] Add signal mask restoration after drain in `platform_timer_destroy()` in `src/spprof/_ext/platform/darwin.c`
- [X] T010 [US2] Rebuild C extension with `pip install -e .`
- [X] T011 [US2] Validate US2 by running quickstart.md "Test Rapid Start/Stop Cycles" script on macOS

**Checkpoint**: User Story 2 complete - macOS profiler shuts down race-free

---

## Phase 4: User Story 3 - Repository Cleanup (Priority: P3)

**Goal**: Remove leftover backup files from repository

**Independent Test**: Verify no `.backup` files in `src/` directory

### Implementation for User Story 3

- [X] T012 [US3] Delete `src/spprof/_ext/platform/linux.c.backup`
- [X] T013 [US3] Verify clean state with `git status` and `find src/ -name "*.backup"`

**Checkpoint**: User Story 3 complete - repository is clean

---

## Phase 5: Polish & Validation

**Purpose**: Final validation across all platforms

- [X] T014 Run full test suite: `pytest tests/ -v`
- [X] T015 [P] Run platform-specific tests: `pytest tests/test_platform.py -v`
- [X] T016 [P] Run profiler tests: `pytest tests/test_profiler.py -v`
- [X] T017 Verify all quickstart.md validation steps pass
- [X] T018 [P] Update docs/ARCHITECTURE.md if any clarifications needed for macOS signal handling

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies - can start immediately
- **User Stories (Phase 2-4)**: Can proceed after Phase 1 verification
  - US1, US2, US3 can proceed in parallel (different files)
- **Polish (Phase 5)**: Depends on all user stories complete

### User Story Dependencies

- **User Story 1 (P1)**: Modifies `src/spprof/__init__.py` only - no dependencies
- **User Story 2 (P2)**: Modifies `src/spprof/_ext/platform/darwin.c` only - no dependencies on US1
- **User Story 3 (P3)**: Deletes backup file - no dependencies on US1/US2

### Within Each User Story

- Implementation tasks are sequential within each story
- Validation task must follow implementation

### Parallel Opportunities

**Phase 1**: T001 and T002 can run in parallel

**User Stories**: All three user stories can run in parallel:
```text
Developer A: T003 → T004 → T005 → T006 (US1 - Python API)
Developer B: T007 → T008 → T009 → T010 → T011 (US2 - darwin.c)
Developer C: T012 → T013 (US3 - cleanup)
```

**Phase 5**: T014, T015, T016, T018 can all run in parallel

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1: Setup (T001-T002)
2. Complete Phase 2: User Story 1 (T003-T006)
3. **STOP and VALIDATE**: Test statistics API independently
4. This delivers core value: users can see accurate profiling statistics

### Incremental Delivery

1. Setup → Foundation ready
2. Add User Story 1 → Stats API works → **MVP!**
3. Add User Story 2 → macOS race-free → Production-ready on macOS
4. Add User Story 3 → Clean repo → Maintenance complete
5. Polish → All validated

### Single Developer Sequence

```text
Phase 1 (Setup) → Phase 2 (US1) → Phase 3 (US2) → Phase 4 (US3) → Phase 5 (Polish)
```

---

## Summary

| Phase | Tasks | Parallel Opportunities |
|-------|-------|----------------------|
| Setup | T001-T002 | 2 parallel |
| US1 (P1) | T003-T006 | 0 (sequential) |
| US2 (P2) | T007-T011 | 0 (sequential) |
| US3 (P3) | T012-T013 | 0 (sequential) |
| Polish | T014-T018 | 4 parallel |
| **Total** | **18** | **6** |

### Task Count by User Story

- **US1**: 4 tasks (MVP - statistics API)
- **US2**: 5 tasks (macOS race fix)
- **US3**: 2 tasks (cleanup)
- **Setup/Polish**: 7 tasks

### MVP Scope

**Minimum Viable Product = User Story 1 (Tasks T001-T006)**:
- Wire dropped_samples from native to Python
- Implement overhead estimation
- Validates core statistics API improvement

---

## Notes

- [P] tasks = different files, no dependencies
- [Story] label maps task to specific user story for traceability
- Each user story is independently testable after completion
- Existing tests must pass after each phase (backward compatibility)
- US2 requires macOS for full validation; can be developed on Linux but tested on macOS
- Commit after each user story completes

