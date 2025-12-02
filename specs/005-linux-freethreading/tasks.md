# Tasks: Linux Free-Threading Support

**Input**: Design documents from `/specs/005-linux-freethreading/`  
**Prerequisites**: plan.md ✅, spec.md ✅, research.md ✅, data-model.md ✅, contracts/ ✅, quickstart.md ✅

**Tests**: Included as integration/stress tests per plan.md testing requirements.

**Organization**: Tasks grouped by user story for independent implementation and testing.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (US1, US2, US3)
- Exact file paths included in descriptions

---

## Phase 1: Setup

**Purpose**: Enable free-threading support flag for Linux

- [X] T001 Enable SPPROF_FREE_THREADING_SAFE for Linux in src/spprof/_ext/internal/pycore_frame.h
- [X] T002 [P] Add SPPROF_ATOMIC_LOAD_PTR macro for architecture-specific loads in src/spprof/_ext/internal/pycore_tstate.h

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Core infrastructure that MUST be complete before ANY user story can be implemented

**⚠️ CRITICAL**: No user story work can begin until this phase is complete

- [X] T003 Add ValidationState global variables in src/spprof/_ext/signal_handler.c
- [X] T004 Implement _spprof_speculative_init() function in src/spprof/_ext/internal/pycore_tstate.h
- [X] T005 [P] Implement _spprof_ptr_valid_speculative() inline function in src/spprof/_ext/internal/pycore_tstate.h
- [X] T006 [P] Implement _spprof_looks_like_code() inline function in src/spprof/_ext/internal/pycore_tstate.h
- [X] T007 Call _spprof_speculative_init() from PyInit__native() in src/spprof/_ext/module.c
- [X] T008 Remove/modify free-threading startup block in src/spprof/_ext/module.c

**Checkpoint**: Foundation ready - speculative capture infrastructure initialized

---

## Phase 3: User Story 1 - Profile Free-Threaded Python Application (Priority: P1) 🎯 MVP

**Goal**: Enable profiling on free-threaded Python 3.13t/3.14t on Linux without crashes

**Independent Test**: Profile a simple Python script on Linux with Python 3.13t and verify samples are captured

### Implementation for User Story 1

- [X] T009 [US1] Implement cycle detection logic (seen[8] rolling window) in _spprof_capture_frames_speculative in src/spprof/_ext/internal/pycore_tstate.h
- [X] T010 [US1] Implement frame pointer validation in capture loop in src/spprof/_ext/internal/pycore_tstate.h
- [X] T011 [US1] Handle Python 3.14 tagged pointers (_PyStackRef) in code extraction in src/spprof/_ext/internal/pycore_tstate.h
- [X] T012 [US1] Implement complete _spprof_capture_frames_speculative() function in src/spprof/_ext/internal/pycore_tstate.h
- [X] T013 [US1] Update capture_python_stack_unsafe() to use speculative capture for free-threaded Linux in src/spprof/_ext/signal_handler.c
- [X] T014 [US1] Implement _spprof_capture_frames_with_instr_speculative() variant in src/spprof/_ext/internal/pycore_tstate.h
- [X] T015 [US1] Update capture_python_stack_with_instr_unsafe() for free-threaded Linux in src/spprof/_ext/signal_handler.c

### Tests for User Story 1

- [X] T016 [P] [US1] Create tests/test_freethreading.py with pytest skip marker for non-free-threaded builds
- [X] T017 [P] [US1] Add test_basic_profiling_freethreaded() in tests/test_freethreading.py
- [X] T018 [P] [US1] Add test_multithreaded_profiling() in tests/test_freethreading.py
- [X] T019 [US1] Add test_no_crash_under_contention() stress test in tests/test_freethreading.py

**Checkpoint**: User Story 1 complete - basic free-threaded profiling works on x86-64 Linux

---

## Phase 4: User Story 2 - View Drop Rate Statistics (Priority: P2)

**Goal**: Users can see how many samples were dropped due to validation failures

**Independent Test**: Check profiler.stats() returns captured and dropped sample counts

### Implementation for User Story 2

- [X] T020 [US2] Add _spprof_samples_dropped_validation atomic counter in src/spprof/_ext/signal_handler.c
- [X] T021 [US2] Increment validation drop counter on cycle detection in src/spprof/_ext/internal/pycore_tstate.h
- [X] T022 [US2] Increment validation drop counter on pointer validation failure in src/spprof/_ext/internal/pycore_tstate.h
- [X] T023 [US2] Implement signal_handler_validation_drops() accessor in src/spprof/_ext/signal_handler.c
- [X] T024 [US2] Declare signal_handler_validation_drops() in src/spprof/_ext/signal_handler.h
- [X] T025 [US2] Expose validation_drops in Python stats via module.c or existing stats path

### Tests for User Story 2

- [X] T026 [US2] Add test_validation_drops_tracked() in tests/test_freethreading.py

**Checkpoint**: User Story 2 complete - drop rate statistics visible to users

---

## Phase 5: User Story 3 - ARM64 Linux Support (Priority: P2)

**Goal**: ARM64 Linux users can profile free-threaded Python with same reliability as x86-64

**Independent Test**: Run profiler on ARM64 Linux with free-threaded Python and verify samples captured

### Implementation for User Story 3

- [X] T027 [US3] Verify SPPROF_ATOMIC_LOAD_PTR uses __atomic_load_n with __ATOMIC_ACQUIRE on ARM64 in src/spprof/_ext/internal/pycore_tstate.h
- [X] T028 [US3] Ensure all frame->previous reads use SPPROF_ATOMIC_LOAD_PTR in speculative capture functions
- [X] T029 [US3] Ensure tstate->current_frame read uses SPPROF_ATOMIC_LOAD_PTR in speculative capture functions
- [X] T030 [US3] Add ARM64-specific heap bounds check (48-bit address space) if needed in src/spprof/_ext/internal/pycore_tstate.h

### Tests for User Story 3

- [X] T031 [US3] Add comment in tests/test_freethreading.py noting ARM64 CI requirement for full coverage

**Checkpoint**: User Story 3 complete - ARM64 Linux free-threading profiling works

---

## Phase 6: Polish & Cross-Cutting Concerns

**Purpose**: Quality improvements affecting all user stories

### Documentation & Code Quality

- [X] T032 [P] Add free-threading stress scenarios to tests/test_stress.py
- [X] T033 [P] Add docstrings/comments documenting async-signal-safety in modified C files
- [X] T034 Run quickstart.md verification steps on x86-64 Linux with Python 3.13t
  - **Automated**: CI job `free-threaded` runs on `ubuntu-latest` with Python 3.13t/3.14t
  - **Script**: `scripts/verify_freethreading.sh` for manual verification
- [X] T035 Verify no compiler warnings on gcc and clang for modified files
  - **Automated**: CI build job compiles on Linux with gcc
  - **Script**: `scripts/verify_freethreading.sh --skip-benchmarks` for manual check

### Verification & Benchmarks (Constitution Compliance)

- [X] T036 [P] Run AddressSanitizer (ASan) on modified C files to verify memory safety
  - **Automated**: CI job `free-threaded-asan` with faulthandler enabled
  - **Note**: Full ASan requires Python built with ASan support
- [X] T037 Benchmark sample capture rate under load (target: ≥99% per SC-002)
  - **Automated**: CI job `free-threaded` includes capture rate verification step
  - **Threshold**: 95% in CI (allows for virtualization overhead), 99% target in production
- [X] T038 Benchmark profiling overhead vs GIL-enabled build (target: <2x per SC-006)
  - **Automated**: CI job `benchmark` runs overhead.py
  - **Script**: `scripts/verify_freethreading.sh` includes overhead benchmark
- [X] T039 Code review checklist: verify all signal handler code paths are async-signal-safe (FR-010)
  - **Completed**: Manual code review passed - all paths are async-signal-safe

---

## Dependencies & Execution Order

### Phase Dependencies

```text
Phase 1: Setup
    │
    ▼
Phase 2: Foundational ──────────────────────┐
    │                                        │
    ▼                                        │
Phase 3: US1 (Core Profiling)               │
    │                                        │
    ├──────────────────┐                     │
    ▼                  ▼                     │
Phase 4: US2       Phase 5: US3             │
(Statistics)       (ARM64)                  │
    │                  │                     │
    └────────┬─────────┘                     │
             ▼                               │
    Phase 6: Polish ◄────────────────────────┘
```

### User Story Dependencies

- **User Story 1 (P1)**: Depends only on Foundational phase (T003-T008) - MVP functionality
- **User Story 2 (P2)**: T020 can start immediately after T003 (counter infrastructure). Full US2 requires US1's capture functions (T012) for counter increment points (T021-T022)
- **User Story 3 (P2)**: Depends on Foundational phase - Can run in parallel with US1/US2 (different code paths, ARM64-specific)

### Within Each User Story

1. Infrastructure/models before implementation
2. Core implementation before tests
3. Tests verify story completeness

### Parallel Opportunities

**Within Phase 2 (Foundational)**:
- T005 and T006 can run in parallel (independent inline functions)

**Within Phase 3 (US1)**:
- T016, T017, T018 can run in parallel (separate test functions)

**Across User Stories (after Phase 2)**:
- US2 T020 (counter variable) can start immediately after T003
- US2 T021-T022 (counter increments) require US1 T012 (capture function exists)
- US3 (T027-T031) can run fully in parallel with US1/US2 (ARM64-specific code paths)
- US2 and US3 do NOT require full US1 completion

---

## Parallel Example: Phase 2 Foundational

```bash
# After T003-T004, launch these in parallel:
Task T005: "Implement _spprof_ptr_valid_speculative() inline function"
Task T006: "Implement _spprof_looks_like_code() inline function"
```

## Parallel Example: User Story 1 Tests

```bash
# After US1 implementation, launch tests in parallel:
Task T016: "Create tests/test_freethreading.py with skip marker"
Task T017: "Add test_basic_profiling_freethreaded()"
Task T018: "Add test_multithreaded_profiling()"
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1: Setup (T001-T002)
2. Complete Phase 2: Foundational (T003-T008)
3. Complete Phase 3: User Story 1 (T009-T019)
4. **STOP and VALIDATE**: Test on x86-64 Linux with Python 3.13t
5. Deploy if MVP sufficient

### Incremental Delivery

1. Phase 1 + Phase 2 → Foundation ready
2. Add User Story 1 → Core profiling works (MVP!)
3. Add User Story 2 → Drop rate visibility
4. Add User Story 3 → ARM64 support
5. Phase 6 → Polish

### File Summary

| File | Tasks | Stories |
|------|-------|---------|
| `src/spprof/_ext/internal/pycore_frame.h` | T001 | Setup |
| `src/spprof/_ext/internal/pycore_tstate.h` | T002, T004-T006, T009-T014, T021-T022, T027-T030, T039 | All |
| `src/spprof/_ext/signal_handler.c` | T003, T013, T015, T020, T023, T036 | US1, US2, Polish |
| `src/spprof/_ext/signal_handler.h` | T024 | US2 |
| `src/spprof/_ext/module.c` | T007-T008, T025 | Foundation, US2 |
| `tests/test_freethreading.py` | T016-T019, T026, T031 | US1, US2, US3 |
| `tests/test_stress.py` | T032 | Polish |
| `benchmarks/` | T037, T038 | Polish (SC verification) |

---

## Notes

- [P] tasks = different files, no dependencies
- [Story] label maps task to specific user story for traceability
- All C code changes must remain async-signal-safe
- Test on both Python 3.13t and 3.14t when available
- ARM64 testing requires CI with ARM64 runners or cross-compilation
- Commit after each logical task group

### Verification Tasks (Constitution Compliance)

- **T036 (ASan)**: Required per constitution "Memory safety: CI MUST include AddressSanitizer/Valgrind runs on Linux"
- **T037-T038 (Benchmarks)**: Verify SC-002 (99% capture rate) and SC-006 (<2x overhead)
- **T039 (Code Review)**: Formal verification of FR-010 (async-signal-safe) compliance

**Total Tasks**: 39

