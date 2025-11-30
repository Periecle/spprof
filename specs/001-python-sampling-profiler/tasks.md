# Tasks: Python Sampling Profiler

**Input**: Design documents from `/specs/001-python-sampling-profiler/`  
**Prerequisites**: plan.md ✓, spec.md ✓, research.md ✓, data-model.md ✓, contracts/ ✓

**Tests**: Test tasks included per constitution requirement (Testing Requirements section).

**Organization**: Tasks grouped by user story from spec.md. Each story is independently testable.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (US1, US2, US3)
- All file paths relative to repository root

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Project initialization and C extension build configuration

- [X] T001 Create project directory structure per plan.md: `src/spprof/`, `src/spprof/_native/`, `src/spprof/_native/platform/`, `src/spprof/_native/compat/`, `tests/`, `benchmarks/`
- [X] T002 Create `pyproject.toml` with hatchling build backend, C extension config, and dependencies (pytest)
- [X] T003 [P] Create `README.md` with project overview and installation instructions
- [X] T004 [P] Create `.gitignore` for Python/C projects (*.so, *.o, __pycache__, build/, dist/)
- [X] T005 [P] Create empty `src/spprof/__init__.py` with version string placeholder
- [X] T006 [P] Create `src/spprof/_profiler.pyi` type stub skeleton

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Core C infrastructure that ALL user stories depend on

**⚠️ CRITICAL**: No user story work can begin until this phase is complete

- [X] T007 Create `src/spprof/_native/ringbuffer.h` with RawSample struct (timestamp, thread_id, depth, frames[128]) and RingBuffer struct (write_idx, read_idx, samples[65536], capacity)
- [X] T008 Implement `src/spprof/_native/ringbuffer.c` with ringbuffer_create(), ringbuffer_destroy(), ringbuffer_write() (async-signal-safe), ringbuffer_read(), ringbuffer_has_data(), ringbuffer_dropped_count()
- [X] T009 [P] Create `src/spprof/_native/framewalker.h` with RawFrameInfo struct and FrameWalkerVTable function pointers
- [X] T010 [P] Create `src/spprof/_native/resolver.h` with ResolvedFrame and ResolvedSample structs
- [X] T011 [P] Create `src/spprof/_native/platform/platform.h` with platform abstraction: platform_timer_create(), platform_timer_destroy(), platform_thread_id(), platform_monotonic_ns()
- [X] T012 Create `src/spprof/_native/module.c` skeleton with PyModuleDef, method table stubs (_start, _stop, _is_active, _get_stats), and PyInit__native()
- [X] T013 Verify C extension compiles with `pip install -e .` (empty module loads in Python)

**Checkpoint**: Foundation ready - C extension skeleton builds and imports

---

## Phase 3: User Story 1 - Profile Production Application (Priority: P1) 🎯 MVP

**Goal**: Working single-threaded profiler on Linux with Speedscope output

**Independent Test**: Import spprof, start profiling, run CPU-bound loop, stop, verify JSON output opens in Speedscope

### Tests for User Story 1

- [X] T014 [P] [US1] Create `tests/test_profiler.py` with test_start_stop_basic() - verify start/stop cycle completes without error
- [X] T015 [P] [US1] Create `tests/test_output.py` with test_speedscope_format() - verify output matches Speedscope JSON schema

### Implementation for User Story 1

- [X] T016 [P] [US1] Create `src/spprof/_native/compat/py312.h` with Python 3.12 frame structures (_PyInterpreterFrame access, tagged pointer macros UNTAG_EXECUTABLE)
- [X] T017 [US1] Implement `src/spprof/_native/framewalker.c` with py312 frame walker: get_current_frame(), get_previous_frame(), get_code_addr(), is_shim_frame(), framewalker_init(), framewalker_capture()
- [X] T018 [US1] Implement `src/spprof/_native/platform/linux.c` with timer_create() using SIGEV_THREAD_ID, CLOCK_THREAD_CPUTIME_ID, platform_timer_create/destroy, platform_thread_id, platform_monotonic_ns
- [X] T019 [US1] Implement `src/spprof/_native/sampler.c` with signal handler (async-signal-safe: capture frames to ring buffer), sampler_init(), sampler_start(), sampler_stop(), sampler_state(), SIGPROF handler chaining
- [X] T020 [US1] Implement `src/spprof/_native/resolver.c` with resolver_init() (starts consumer thread), resolver_shutdown(), resolver_get_samples(), symbol resolution with GIL, LRU cache for PyCodeObject → strings
- [X] T021 [US1] Complete `src/spprof/_native/module.c` with _start() calling sampler_start + resolver_init, _stop() calling sampler_stop + resolver_get_samples + return Python list, _is_active(), _get_stats()
- [X] T022 [US1] Create Python data classes in `src/spprof/__init__.py`: Frame, Sample, Profile, ProfilerStats per contracts/python-api.md
- [X] T023 [US1] Implement start(), stop(), is_active(), stats() functions in `src/spprof/__init__.py` wrapping _native calls
- [X] T024 [US1] Implement `src/spprof/output.py` with to_speedscope() method producing Speedscope JSON format per data-model.md
- [X] T025 [US1] Add Profile.save() method in `src/spprof/__init__.py` supporting format="speedscope"
- [X] T026 [US1] Add Profiler context manager class in `src/spprof/__init__.py`
- [X] T027 [US1] Add @profile decorator in `src/spprof/__init__.py`
- [X] T028 [US1] Expand `tests/test_profiler.py`: test_profiler_context_manager(), test_profile_decorator(), test_cpu_bound_captures_samples()
- [X] T029 [US1] Expand `tests/test_output.py`: test_profile_save_file(), test_zero_samples_valid_output()

**Checkpoint**: User Story 1 complete - single-threaded profiling works on Linux with Python 3.12, Speedscope output valid

---

## Phase 4: User Story 2 - Profile Multi-threaded Application (Priority: P2)

**Goal**: Profile all threads, add macOS and Windows support

**Independent Test**: Run 5-thread workload, verify all thread IDs appear in output, test on macOS/Windows

### Tests for User Story 2

- [X] T030 [P] [US2] Create `tests/test_threading.py` with test_multi_thread_sampling() - 5 threads, verify samples from each
- [X] T031 [P] [US2] Add test_thread_ids_in_output() to `tests/test_threading.py` - verify thread_id field populated

### Implementation for User Story 2

- [X] T032 [P] [US2] Implement `src/spprof/_native/platform/darwin.c` with setitimer(ITIMER_PROF) for macOS
- [X] T033 [P] [US2] Implement `src/spprof/_native/platform/windows.c` with CreateTimerQueueTimer + suspend-and-sample model
- [X] T034 [US2] Update `src/spprof/_native/sampler.c` with per-thread timer registration for Linux (timer_create per thread)
- [X] T035 [US2] Add _register_thread() to `src/spprof/_native/module.c` for explicit thread registration
- [X] T036 [US2] Update `src/spprof/__init__.py` with thread registration hook in threading.Thread.start monkey-patch
- [X] T037 [US2] Update `src/spprof/output.py` to_speedscope() to group samples by thread (separate profiles array entries)
- [X] T038 [US2] Add to_collapsed() method in `src/spprof/output.py` for FlameGraph compatibility
- [X] T039 [US2] Expand `tests/test_threading.py`: test_thread_terminates_during_profiling(), test_main_thread_blocked_other_sampled()
- [X] T040 [US2] Add `tests/test_platform.py` with platform detection and timer mechanism tests

**Checkpoint**: User Story 2 complete - multi-threaded profiling works on Linux/macOS/Windows

---

## Phase 5: User Story 3 - Profile Across Python Versions (Priority: P4)

**Goal**: Support Python 3.9, 3.10, 3.11, 3.12, 3.13, 3.14

**Independent Test**: Run same profiling script on each Python version, verify output generated

### Tests for User Story 3

- [X] T041 [P] [US3] Create `tests/test_version_compat.py` with test_frame_walker_version_detection() - verify correct compat header used

### Implementation for User Story 3

- [X] T042 [P] [US3] Create `src/spprof/_native/compat/py39.h` with Python 3.9-3.10 PyFrameObject access (f_code, f_back, f_lineno direct fields)
- [X] T043 [P] [US3] Create `src/spprof/_native/compat/py311.h` with Python 3.11 _PyInterpreterFrame (cframe->current_frame, frame->previous, frame->owner check)
- [X] T044 [P] [US3] Update `src/spprof/_native/compat/py312.h` with tagged pointer handling (f_executable bit masking)
- [X] T045 [P] [US3] Create `src/spprof/_native/compat/py313.h` with free-threaded build support (_Py_atomic_load_ptr for frame access, Py_GIL_DISABLED check)
- [X] T046 [P] [US3] Create `src/spprof/_native/compat/py314.h` with tail-call interpreter compatibility notes (verify _PyInterpreterFrame linkage preserved)
- [X] T047 [US3] Update `src/spprof/_native/framewalker.c` with compile-time PY_VERSION_HEX dispatch to include appropriate compat header
- [X] T048 [US3] Update `src/spprof/_native/resolver.c` line number computation for 3.10+ (PyCode_Addr2Line) vs 3.9 (co_lnotab parsing)
- [X] T049 [US3] Create `.github/workflows/ci.yml` with Python version matrix (3.9, 3.10, 3.11, 3.12, 3.13, 3.14-dev), platform matrix (ubuntu, macos, windows)
- [X] T050 [US3] Add Python 3.13 free-threaded build to CI matrix (--disable-gil)
- [X] T051 [US3] Expand `tests/test_version_compat.py`: test_profile_on_current_version(), test_frame_structure_access()

**Checkpoint**: User Story 3 complete - profiler works on Python 3.9–3.14 across all platforms

---

## Phase 6: Polish & Cross-Cutting Concerns

**Purpose**: Production hardening, safety verification, documentation

- [X] T052 [P] Create `tests/test_signal_safety.py` with test_no_deadlock_during_gil_operations() using watchdog thread (5s timeout)
- [X] T053 [P] Add test_gc_stress() to `tests/test_signal_safety.py` - profile while GC runs, verify no crashes
- [X] T054 [P] Add test_ring_buffer_overflow() to `tests/test_signal_safety.py` - high-frequency sampling, verify drops not crashes
- [X] T055 [P] Create `benchmarks/overhead.py` with measure_overhead() at 1ms, 10ms, 100ms intervals, report percentage
- [X] T056 Update `.github/workflows/ci.yml` with AddressSanitizer build for Linux
- [X] T057 [P] Complete `src/spprof/_profiler.pyi` type stubs with all public API signatures
- [X] T058 [P] Add memory_limit_mb parameter to start() in `src/spprof/__init__.py` with default 100MB per clarifications
- [X] T059 Update `README.md` with usage examples from quickstart.md
- [X] T060 [P] Create `examples/basic_profile.py` demonstrating start/stop/save workflow
- [X] T061 [P] Create `examples/threaded_profile.py` demonstrating multi-threaded profiling
- [X] T062 Run quickstart.md validation - verify all code examples work
- [ ] T063 [P] Create K8s integration test with restricted securityContext (no SYS_PTRACE, non-root) in `tests/test_k8s.py`
- [X] T064 [P] [US1] Implement optional C-stack unwinding via libunwind in `src/spprof/_native/unwind.c` (decoupled, Linux only)
- [X] T065 [P] [US1] Create `src/spprof/_native/unwind.h` with unwind_capture() for native frame collection
- [X] T066 [US1] Integrate libunwind frames with Python frames in `src/spprof/_native/framewalker.c` (mixed-mode stacks)

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies - can start immediately
- **Foundational (Phase 2)**: Depends on Setup completion - BLOCKS all user stories
- **User Stories (Phase 3–5)**: All depend on Foundational phase completion
  - US1 (Phase 3): Can start after Foundational
  - US2 (Phase 4): Can start after Foundational; benefits from US1 components but independently testable
  - US3 (Phase 5): Can start after Foundational; requires US1 framewalker base
- **Polish (Phase 6)**: Depends on at least US1 complete

### User Story Dependencies

- **User Story 1 (P1)**: Core MVP - enables single-threaded profiling, no dependencies on other stories
- **User Story 2 (P2)**: Adds threading/platforms - builds on US1 sampler.c but independently testable
- **User Story 3 (P4)**: Adds version compat - builds on US1 framewalker.c but independently testable

### Within Each User Story

- Tests FIRST (T014-T015, T030-T031, T041)
- C layer before Python layer
- Core before conveniences (start/stop before context manager/decorator)
- Output format before save functionality

### Parallel Opportunities

**Phase 1** (all parallel after T001):
```
T002, T003, T004, T005, T006 - can run simultaneously
```

**Phase 2** (after T007-T008):
```
T009, T010, T011 - can run simultaneously (header files)
```

**Phase 3 US1** (after T012-T013):
```
T014, T015, T016 - tests and compat header in parallel
```

**Phase 4 US2**:
```
T030, T031, T032, T033 - tests and platform files in parallel
```

**Phase 5 US3**:
```
T041, T042, T043, T044, T045, T046 - all compat headers in parallel
```

**Phase 6**:
```
T052, T053, T054, T055, T057, T060, T061 - independent safety/doc tasks
```

---

## Parallel Example: User Story 1

```bash
# After foundational is done, launch in parallel:
Task T014: "Create tests/test_profiler.py with test_start_stop_basic()"
Task T015: "Create tests/test_output.py with test_speedscope_format()"
Task T016: "Create src/spprof/_native/compat/py312.h"

# After T016 completes:
Task T017: "Implement framewalker.c with py312 frame walker"
Task T018: "Implement platform/linux.c with timer_create"  # Can parallel with T017

# After T017 + T018:
Task T019: "Implement sampler.c with signal handler"
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1: Setup
2. Complete Phase 2: Foundational (CRITICAL - blocks all stories)
3. Complete Phase 3: User Story 1
4. **STOP and VALIDATE**: Test with `python -c "import spprof; spprof.start(); ...; spprof.stop().save('test.json')"`
5. Verify JSON opens in https://www.speedscope.app

### Incremental Delivery

1. **Setup + Foundational** → Ring buffer and module skeleton ready
2. **Add User Story 1** → Single-thread Linux profiling works → **MVP!**
3. **Add User Story 2** → Multi-thread + macOS/Windows → Full platform support
4. **Add User Story 3** → Python 3.9–3.14 → Full version support
5. **Polish** → Production-ready with safety tests

### Suggested MVP Scope

**Minimum Viable Product = User Story 1 complete (Tasks T001–T029)**:
- Single-threaded profiling
- Linux only (Python 3.12)
- Speedscope JSON output
- Basic Python API (start/stop/save)

This delivers core value: developers can profile Python applications and generate flame graphs.

---

## Notes

- [P] tasks = different files, no dependencies
- [Story] label maps task to specific user story for traceability
- Each user story is independently completable and testable
- Commit after each task or logical group
- Stop at any checkpoint to validate story independently
- Constitution requires AddressSanitizer in CI (T056)
- Memory limit default changed to 100MB per clarifications session
- libunwind tasks (T064-T066) are optional/decoupled; MVP works without C-stack unwinding
- K8s integration test (T063) verifies restricted securityContext operation

---

## Summary

| Phase | Tasks | Parallel Opportunities |
|-------|-------|----------------------|
| Setup | T001–T006 | 5 parallel after T001 |
| Foundational | T007–T013 | 3 parallel headers |
| US1 (MVP) | T014–T029 | Tests + compat parallel |
| US2 (Threading) | T030–T040 | Tests + platforms parallel |
| US3 (Versions) | T041–T051 | All compat headers parallel |
| Polish | T052–T066 | Most tasks parallel |

**Total Tasks**: 66  
**MVP Tasks**: 29 (through US1, excludes optional libunwind T064-T066)  
**Independently Testable Stories**: 3

