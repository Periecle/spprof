# Tasks: Internal API Only Frame Walking

**Input**: Design documents from `/specs/004-internal-api-only/`  
**Prerequisites**: plan.md, spec.md, research.md, data-model.md, contracts/internal-frame-api.md, quickstart.md

**Tests**: Tests not explicitly requested - included as optional validation tasks in Polish phase.

**Organization**: Tasks grouped by user story to enable independent implementation and testing.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (US1, US2, US3)
- Include exact file paths in descriptions

## Path Conventions

- **Single project**: `src/spprof/_ext/` for C extension sources
- **Tests**: `tests/` at repository root

---

## Phase 1: Setup

**Purpose**: Prepare the codebase for internal API expansion

- [X] T001 Backup current working state with git commit if uncommitted changes exist
- [X] T002 Review existing `internal/pycore_frame.h` structure in `src/spprof/_ext/internal/pycore_frame.h`
- [X] T003 Review existing `internal/pycore_tstate.h` functions in `src/spprof/_ext/internal/pycore_tstate.h`

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Update version detection and shared infrastructure before adding version-specific code

**⚠️ CRITICAL**: User story implementation depends on these foundational changes

- [X] T004 Add `SPPROF_PY39` and `SPPROF_PY310` version detection macros in `src/spprof/_ext/internal/pycore_frame.h`
- [X] T005 Update minimum version error check from 3.11 to 3.9 in `src/spprof/_ext/internal/pycore_frame.h`
- [X] T006 Add `#include <frameobject.h>` conditional for Python 3.9 `PyTryBlock` in `src/spprof/_ext/internal/pycore_frame.h`

**Checkpoint**: Version detection ready - user story implementation can now begin

---

## Phase 3: User Story 1 - Consistent Profiling Details (Priority: P1) 🎯 MVP

**Goal**: Python 3.9 and 3.10 capture full frame details (code objects, instruction pointers, frame ownership) identical to Python 3.11+

**Independent Test**: Profile a sample app on Python 3.9/3.10 and verify captured data includes code pointers, instruction pointers, and ownership flags

### Implementation for User Story 1

#### Python 3.9 Support

- [X] T007 [P] [US1] Add `_spprof_PyFrameObject` struct definition for Python 3.9 in `src/spprof/_ext/internal/pycore_frame.h`
- [X] T008 [P] [US1] Add `_spprof_get_current_frame()` accessor for Python 3.9 (using `tstate->frame`) in `src/spprof/_ext/internal/pycore_frame.h`
- [X] T009 [P] [US1] Add `_spprof_frame_get_previous()` accessor for Python 3.9 (using `f_back`) in `src/spprof/_ext/internal/pycore_frame.h`
- [X] T010 [P] [US1] Add `_spprof_frame_get_code()` accessor for Python 3.9 (using `f_code`) in `src/spprof/_ext/internal/pycore_frame.h`
- [X] T011 [P] [US1] Add `_spprof_frame_get_instr_ptr()` accessor for Python 3.9 (compute from `f_lasti`) in `src/spprof/_ext/internal/pycore_frame.h`
- [X] T012 [P] [US1] Add `_spprof_frame_is_shim()` accessor for Python 3.9 (always returns 0) in `src/spprof/_ext/internal/pycore_frame.h`
- [X] T013 [P] [US1] Add `_spprof_frame_get_owner()` accessor for Python 3.9 (infer from code flags) in `src/spprof/_ext/internal/pycore_frame.h`

#### Python 3.10 Support

- [X] T014 [P] [US1] Add `_spprof_PyFrameObject` struct definition for Python 3.10 in `src/spprof/_ext/internal/pycore_frame.h`
- [X] T015 [P] [US1] Add `_spprof_PyFrameState` enum for Python 3.10 in `src/spprof/_ext/internal/pycore_frame.h`
- [X] T016 [P] [US1] Add `_spprof_get_current_frame()` accessor for Python 3.10 (using `tstate->frame`) in `src/spprof/_ext/internal/pycore_frame.h`
- [X] T017 [P] [US1] Add `_spprof_frame_get_previous()` accessor for Python 3.10 in `src/spprof/_ext/internal/pycore_frame.h`
- [X] T018 [P] [US1] Add `_spprof_frame_get_code()` accessor for Python 3.10 in `src/spprof/_ext/internal/pycore_frame.h`
- [X] T019 [P] [US1] Add `_spprof_frame_get_instr_ptr()` accessor for Python 3.10 (compute from `f_lasti`) in `src/spprof/_ext/internal/pycore_frame.h`
- [X] T020 [P] [US1] Add `_spprof_frame_is_shim()` accessor for Python 3.10 (always returns 0) in `src/spprof/_ext/internal/pycore_frame.h`
- [X] T021 [P] [US1] Add `_spprof_frame_get_owner()` accessor for Python 3.10 (using `f_gen_or_coro`) in `src/spprof/_ext/internal/pycore_frame.h`

#### Thread State Updates

- [X] T022 [US1] Update `_spprof_tstate_get()` to support Python 3.9/3.10 in `src/spprof/_ext/internal/pycore_tstate.h`
- [X] T023 [US1] Update `_spprof_capture_frames_unsafe()` to handle 3.9/3.10 frame types in `src/spprof/_ext/internal/pycore_tstate.h`
- [X] T024 [US1] Update `_spprof_capture_frames_with_instr_unsafe()` for 3.9/3.10 in `src/spprof/_ext/internal/pycore_tstate.h`
- [X] T025 [US1] Update `_spprof_capture_frames_from_tstate()` for 3.9/3.10 in `src/spprof/_ext/internal/pycore_tstate.h`

**Checkpoint**: Python 3.9/3.10 internal API support complete - can be tested independently

---

## Phase 4: User Story 2 - Simplified Codebase Maintenance (Priority: P2)

**Goal**: Single frame walking implementation - remove dual public/internal API code paths

**Independent Test**: Verify codebase has single implementation path; `SPPROF_USE_INTERNAL_API` flag has no effect

### Implementation for User Story 2

#### Remove Conditional Compilation in framewalker.c

- [X] T026 [US2] Remove `#ifdef SPPROF_USE_INTERNAL_API` block in `src/spprof/_ext/framewalker.c`
- [X] T027 [US2] Remove `#else` public API fallback code in `src/spprof/_ext/framewalker.c`
- [X] T028 [US2] Update includes to always use internal headers in `src/spprof/_ext/framewalker.c`
- [X] T029 [US2] Remove `FRAMEWALKER_MODE` conditional logic in `src/spprof/_ext/framewalker.c`
- [X] T030 [US2] Remove `public_get_current_frame()` and related functions in `src/spprof/_ext/framewalker.c`
- [X] T031 [US2] Update `framewalker_init()` to remove public API vtable setup in `src/spprof/_ext/framewalker.c`
- [X] T032 [US2] Update `g_version_info` string to remove "public-api" references in `src/spprof/_ext/framewalker.c`

#### Delete Compatibility Headers

- [X] T033 [P] [US2] Delete `src/spprof/_ext/compat/py39.h`
- [X] T034 [P] [US2] Delete `src/spprof/_ext/compat/py311.h`
- [X] T035 [P] [US2] Delete `src/spprof/_ext/compat/py312.h`
- [X] T036 [P] [US2] Delete `src/spprof/_ext/compat/py313.h`
- [X] T037 [P] [US2] Delete `src/spprof/_ext/compat/py314.h`
- [X] T038 [US2] Delete empty `src/spprof/_ext/compat/` directory

#### Update Build Configuration

- [X] T039 [US2] Remove `SPPROF_USE_INTERNAL_API` from compile definitions in `setup.py`
- [X] T040 [US2] Update version info string in `src/spprof/_ext/module.c` to reflect internal-only mode

**Checkpoint**: Codebase simplified - single implementation path for all Python versions

---

## Phase 5: User Story 3 - Async-Signal-Safe Frame Walking (Priority: P3)

**Goal**: Verify frame walking is async-signal-safe on all Python versions (no `Py_DECREF`, no locks)

**Independent Test**: Run stress tests at 1000 samples/sec on Python 3.9/3.10 without crashes

### Implementation for User Story 3

- [X] T041 [US3] Audit `internal/pycore_frame.h` to verify no Python C API calls in accessors for `src/spprof/_ext/internal/pycore_frame.h`
- [X] T042 [US3] Audit `internal/pycore_tstate.h` to verify no `Py_DECREF`/`Py_INCREF` calls in `src/spprof/_ext/internal/pycore_tstate.h`
- [X] T043 [US3] Add code comments documenting async-signal-safety properties in `src/spprof/_ext/internal/pycore_frame.h`
- [X] T044 [US3] Add code comments documenting async-signal-safety properties in `src/spprof/_ext/internal/pycore_tstate.h`

**Checkpoint**: Async-signal-safety verified for all Python versions

---

## Phase 6: Polish & Cross-Cutting Concerns

**Purpose**: Validation, documentation, and final cleanup

### Struct Validation (Optional but Recommended)

- [X] T045 [P] Add `_Static_assert` for Python 3.9 struct field offsets in `src/spprof/_ext/internal/pycore_frame.h`
- [X] T046 [P] Add `_Static_assert` for Python 3.10 struct field offsets in `src/spprof/_ext/internal/pycore_frame.h`

### Documentation Updates

- [X] T047 [P] Update code comments to remove references to public API mode in `src/spprof/_ext/framewalker.c`
- [X] T048 [P] Update code comments in `src/spprof/_ext/internal/pycore_frame.h` header documentation

### Build Verification

- [X] T049 Verify extension builds successfully on Python 3.9
- [X] T050 Verify extension builds successfully on Python 3.10
- [X] T051 Verify extension builds successfully on Python 3.11
- [X] T052 Verify extension builds successfully on Python 3.12
- [X] T053 Verify extension builds successfully on Python 3.13
- [X] T054 Verify extension builds successfully on Python 3.14 (if available)

### Test Suite Verification

- [X] T055 Run existing test suite on Python 3.9
- [X] T056 Run existing test suite on Python 3.10
- [X] T057 Run existing test suite on Python 3.11+
- [X] T058 Run `quickstart.md` verification commands

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies - can start immediately
- **Foundational (Phase 2)**: Depends on Setup completion - BLOCKS all user stories
- **User Story 1 (Phase 3)**: Depends on Foundational - adds 3.9/3.10 support
- **User Story 2 (Phase 4)**: Depends on US1 completion - removes old code paths
- **User Story 3 (Phase 5)**: Can run in parallel with US2 - validation only
- **Polish (Phase 6)**: Depends on US1 and US2 completion

### User Story Dependencies

- **User Story 1 (P1)**: Can start after Foundational - No dependencies on other stories
- **User Story 2 (P2)**: Depends on US1 - old code can only be removed after new code works
- **User Story 3 (P3)**: Can start after US1 - independent validation of async-signal-safety

### Within Each User Story

- Python 3.9 tasks (T007-T013) can run in parallel
- Python 3.10 tasks (T014-T021) can run in parallel with 3.9 tasks
- Thread state updates (T022-T025) depend on struct definitions
- Delete tasks (T033-T038) can run in parallel

### Parallel Opportunities

- T007-T013 (Python 3.9 struct/accessors) - all parallel
- T014-T021 (Python 3.10 struct/accessors) - all parallel
- T033-T037 (delete compat headers) - all parallel
- T045-T048 (polish tasks) - all parallel
- T049-T058 (verification tasks) - run sequentially per Python version

---

## Parallel Example: User Story 1

```bash
# Launch all Python 3.9 accessor implementations together:
Task T007: "Add _spprof_PyFrameObject struct for Python 3.9"
Task T008: "Add _spprof_get_current_frame() for Python 3.9"
Task T009: "Add _spprof_frame_get_previous() for Python 3.9"
Task T010: "Add _spprof_frame_get_code() for Python 3.9"
Task T011: "Add _spprof_frame_get_instr_ptr() for Python 3.9"
Task T012: "Add _spprof_frame_is_shim() for Python 3.9"
Task T013: "Add _spprof_frame_get_owner() for Python 3.9"

# Launch all Python 3.10 accessor implementations together:
Task T014: "Add _spprof_PyFrameObject struct for Python 3.10"
Task T015: "Add _spprof_PyFrameState enum for Python 3.10"
# ... etc
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1: Setup (T001-T003)
2. Complete Phase 2: Foundational (T004-T006) - **CRITICAL**
3. Complete Phase 3: User Story 1 (T007-T025)
4. **STOP and VALIDATE**: Build and test on Python 3.9/3.10
5. If working, proceed to User Story 2

### Incremental Delivery

1. Setup + Foundational → Ready for implementation
2. User Story 1 → Test on 3.9/3.10 → **MVP Complete!**
3. User Story 2 → Codebase cleanup → Verify all versions
4. User Story 3 → Async-signal-safety verification
5. Polish → Documentation and validation

### Single Developer Strategy

Execute in strict phase order:
1. T001-T003 (Setup)
2. T004-T006 (Foundational)
3. T007-T025 (US1 - add new code)
4. Build + test on 3.9/3.10
5. T026-T040 (US2 - remove old code)
6. T041-T044 (US3 - verify safety)
7. T045-T058 (Polish)

---

## Notes

- [P] tasks = different files or independent operations, no dependencies
- [Story] label maps task to specific user story for traceability
- User Story 1 is the MVP - provides 3.9/3.10 support
- User Story 2 cleans up the codebase - should only be done after US1 is verified working
- User Story 3 is validation - ensures the implementation is async-signal-safe
- All struct definitions must exactly match CPython source for the target version
- Refer to `data-model.md` for exact struct layouts
- Refer to `quickstart.md` for implementation code snippets

