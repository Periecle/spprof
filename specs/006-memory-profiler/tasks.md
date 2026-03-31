# Tasks: Memory Allocation Profiler

**Input**: Design documents from `/specs/006-memory-profiler/`  
**Prerequisites**: plan.md ✓, spec.md ✓, research.md ✓, data-model.md ✓, contracts/ ✓  

**Tests**: Integration and safety tests are included given the complexity and production requirements of this feature.

**Organization**: Tasks organized by foundational infrastructure (required for ALL stories), then by user story priority (P1 → P2 → P3).

## Format: `[ID] [P?] [Story?] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3)
- Include exact file paths in descriptions

## Path Conventions

- **Source**: `src/spprof/_ext/memprof/` (C implementation)
- **Platform**: `src/spprof/_ext/platform/` (platform-specific hooks)
- **Python**: `src/spprof/` (Python wrapper)
- **Tests**: `tests/` (pytest tests)

---

## Platform Support Note

> **Windows Support**: FR-023 (SHOULD) is deferred to v1.1. The plan.md includes `windows_memprof.c` in the project structure and documents Windows as "experimental" in the Risk Register. No implementation tasks are included in this release. See plan.md for details.

---

## Phase 1: Setup

**Purpose**: Project structure and header files

- [x] T001 Create memprof directory structure in `src/spprof/_ext/memprof/`
- [x] T002 [P] Create main header with constants and config in `src/spprof/_ext/memprof/memprof.h`
- [x] T003 [P] Create heap map header with state machine defines in `src/spprof/_ext/memprof/heap_map.h`
- [x] T004 [P] Create stack intern header in `src/spprof/_ext/memprof/stack_intern.h`
- [x] T005 [P] Create bloom filter header in `src/spprof/_ext/memprof/bloom.h`
- [x] T006 [P] Create sampling engine header in `src/spprof/_ext/memprof/sampling.h`
- [x] T007 [P] Create stack capture header in `src/spprof/_ext/memprof/stack_capture.h`
- [x] T008 Update meson.build to include memprof sources in `src/spprof/meson.build`

---

## Phase 2: Foundational (Core C Infrastructure)

**Purpose**: Lock-free data structures and sampling engine required by ALL user stories

**⚠️ CRITICAL**: No user story work can begin until this phase is complete

### 2.1 Heap Map (Lock-Free Hash Table)

- [x] T009 Implement HeapMapEntry packed metadata macros in `src/spprof/_ext/memprof/heap_map.c`
- [x] T010 Implement heap_map_init() with mmap allocation in `src/spprof/_ext/memprof/heap_map.c`
- [x] T011 Implement heap_map_reserve() two-phase insert (EMPTY/TOMBSTONE → RESERVED) in `src/spprof/_ext/memprof/heap_map.c`
- [x] T012 Implement heap_map_finalize() (RESERVED → ptr with CAS) in `src/spprof/_ext/memprof/heap_map.c`
- [x] T013 Implement heap_map_remove() with "death during birth" handling in `src/spprof/_ext/memprof/heap_map.c`
- [x] T014 Implement heap_map_load_percent() and iteration helpers in `src/spprof/_ext/memprof/heap_map.c`
- [x] T015 Implement heap_map_destroy() in `src/spprof/_ext/memprof/heap_map.c`

### 2.2 Bloom Filter (Free Path Optimization)

- [x] T016 Implement bloom_get_indices() double hashing in `src/spprof/_ext/memprof/bloom.c`
- [x] T017 Implement bloom_add() with atomic OR in `src/spprof/_ext/memprof/bloom.c`
- [x] T018 Implement bloom_might_contain() with relaxed loads in `src/spprof/_ext/memprof/bloom.c`
- [x] T019 Implement bloom_init() with mmap allocation in `src/spprof/_ext/memprof/bloom.c`
- [x] T020 Implement bloom_rebuild_from_heap() with intentional leak pattern in `src/spprof/_ext/memprof/bloom.c`
- [x] T021 Implement bloom_cleanup_leaked_filters() for shutdown in `src/spprof/_ext/memprof/bloom.c`

### 2.3 Stack Intern Table

- [x] T022 Implement fnv1a_hash_stack() in `src/spprof/_ext/memprof/stack_intern.c`
- [x] T023 Implement stack_table_init() with dynamic sizing in `src/spprof/_ext/memprof/stack_intern.c`
- [x] T024 Implement stack_table_intern() with CAS on hash field in `src/spprof/_ext/memprof/stack_intern.c`
- [x] T025 Implement stack_table_get() in `src/spprof/_ext/memprof/stack_intern.c`
- [x] T026 Implement stack_table_resize() with platform-specific mmap handling in `src/spprof/_ext/memprof/stack_intern.c`

### 2.4 Sampling Engine

- [x] T027 Implement xorshift128+ PRNG (prng_next, prng_next_double) in `src/spprof/_ext/memprof/sampling.c`
- [x] T028 Implement next_sample_threshold() exponential distribution in `src/spprof/_ext/memprof/sampling.c`
- [x] T029 Implement TLS state initialization with entropy seeding in `src/spprof/_ext/memprof/sampling.c`
- [x] T030 Implement hot path logic (byte counter decrement, branch) in `src/spprof/_ext/memprof/sampling.c`
- [x] T031 Implement cold path logic (sample handling, threshold reset) in `src/spprof/_ext/memprof/sampling.c`
- [x] T032 Implement re-entrancy guard (inside_profiler flag) in `src/spprof/_ext/memprof/sampling.c`

### 2.5 Stack Capture

- [x] T033 Implement platform address validation macros (ADDR_MAX_USER) in `src/spprof/_ext/memprof/stack_capture.c`
- [x] T034 Implement capture_native_stack() frame pointer walker (x86_64) in `src/spprof/_ext/memprof/stack_capture.c`
- [x] T035 [P] Implement capture_native_stack() for ARM64 in `src/spprof/_ext/memprof/stack_capture.c`
- [x] T036 Implement capture_mixed_stack() integrating with framewalker.c in `src/spprof/_ext/memprof/stack_capture.c`
- [x] T037 Implement is_python_interpreter_frame() heuristic in `src/spprof/_ext/memprof/stack_capture.c`
- [x] T038 Implement frame pointer health tracking and warnings in `src/spprof/_ext/memprof/stack_capture.c`

### 2.6 Core Lifecycle

- [x] T039 Implement MemProfGlobalState definition in `src/spprof/_ext/memprof/memprof.c`
- [x] T040 Implement memprof_init() orchestrating all subsystem init in `src/spprof/_ext/memprof/memprof.c`
- [x] T041 Implement memprof_start() setting active flags in `src/spprof/_ext/memprof/memprof.c`
- [x] T042 Implement memprof_stop() (disable alloc, keep free tracking) in `src/spprof/_ext/memprof/memprof.c`
- [x] T043 Implement memprof_shutdown() one-way shutdown in `src/spprof/_ext/memprof/memprof.c`
- [x] T044 Implement memprof_get_snapshot() with acquire loads in `src/spprof/_ext/memprof/memprof.c`
- [x] T045 Implement memprof_get_stats() in `src/spprof/_ext/memprof/memprof.c`
- [x] T046 Implement global sequence counter for ABA detection in `src/spprof/_ext/memprof/memprof.c`

### 2.7 Foundational Tests

- [x] T047 [P] Unit test heap_map concurrent insert/remove in `tests/test_memprof_data_structures.py`
- [x] T048 [P] Unit test stack_table deduplication in `tests/test_memprof_data_structures.py`
- [x] T049 [P] Unit test bloom filter false positive rate in `tests/test_memprof_data_structures.py`
- [x] T050 [P] Unit test PRNG statistical properties in `tests/test_memprof_data_structures.py`
- [x] T051 Concurrent stress test for heap map (10 threads, 1M ops) in `tests/test_memprof_stress.py`

**Checkpoint**: Core C infrastructure complete - platform interposition can now begin

---

## Phase 3: Platform Interposition (macOS First)

**Purpose**: malloc_logger callback enables basic profiling on macOS

### 3.1 macOS malloc_logger

- [x] T052 Implement spprof_malloc_logger() callback in `src/spprof/_ext/platform/darwin_memprof.c`
- [x] T053 Implement memprof_darwin_install() with atomic flag in `src/spprof/_ext/platform/darwin_memprof.c`
- [x] T054 Implement memprof_darwin_remove() with nanosleep delay in `src/spprof/_ext/platform/darwin_memprof.c`
- [x] T055 Implement sequence-based zombie detection in `src/spprof/_ext/platform/darwin_memprof.c`
- [x] T056 Integration test for macOS malloc_logger in `tests/test_darwin_mach.py`

### 3.2 Linux LD_PRELOAD

- [x] T057 Implement bootstrap heap (64KB static buffer) in `src/spprof/_ext/platform/linux_memprof.c`
- [x] T058 Implement ensure_initialized() with dlsym recursion guard in `src/spprof/_ext/platform/linux_memprof.c`
- [x] T059 Implement malloc/calloc/realloc/free interposition in `src/spprof/_ext/platform/linux_memprof.c`
- [x] T060 Implement fail-fast on dlsym failure in `src/spprof/_ext/platform/linux_memprof.c`
- [x] T061 [P] Implement aligned_alloc/memalign/posix_memalign hooks in `src/spprof/_ext/platform/linux_memprof.c`
- [x] T062 Add meson build for libspprof_alloc.so shared library in `src/spprof/meson.build`
- [x] T063 Integration test for Linux LD_PRELOAD in `tests/test_memprof.py`

### 3.3 Platform Abstraction

- [x] T064 Implement platform detection and hook selection in `src/spprof/_ext/memprof/memprof.c`

**Checkpoint**: Platform hooks complete - Python API can now be implemented

---

## Phase 4: User Story 1-3 (P1) - Core Profiling 🎯 MVP

**Goal**: Basic memory profiling, native extension visibility, production-safe operation

**Independent Test**: Start profiler, run NumPy workload, capture snapshot, verify allocation sites with <0.1% overhead

### Tests for User Stories 1-3

- [x] T065 [P] [US1] Integration test for basic start/stop/snapshot cycle in `tests/test_memprof.py`
- [x] T066 [P] [US2] Integration test for NumPy allocation capture in `tests/test_memprof.py`
- [x] T067 [P] [US3] Performance test verifying <0.1% overhead at 512KB rate in `tests/test_memprof.py`
- [x] T068 [P] [US3] Stress test for high allocation rate (1M allocs/sec) in `tests/test_memprof_stress.py`
- [x] T069 [P] [US3] Concurrent allocation test (10 threads) in `tests/test_memprof_stress.py`

### Python Bindings Implementation

- [x] T070 [US1] Add memprof module init to Python extension in `src/spprof/_ext/module.c`
- [x] T071 [US1] Implement _memprof_init() Python binding in `src/spprof/_ext/module.c`
- [x] T072 [US1] Implement _memprof_start() Python binding in `src/spprof/_ext/module.c`
- [x] T073 [US1] Implement _memprof_stop() Python binding in `src/spprof/_ext/module.c`
- [x] T074 [US1] Implement _memprof_get_snapshot() Python binding in `src/spprof/_ext/module.c`
- [x] T075 [US1] Implement _memprof_get_stats() Python binding in `src/spprof/_ext/module.c`
- [x] T076 [US1] Implement _memprof_shutdown() Python binding in `src/spprof/_ext/module.c`

### Python Wrapper Implementation

- [x] T077 [US1] Create AllocationSample dataclass in `src/spprof/memprof.py`
- [x] T078 [US1] Create StackFrame dataclass in `src/spprof/memprof.py`
- [x] T079 [US1] Create HeapSnapshot dataclass with top_allocators() in `src/spprof/memprof.py`
- [x] T080 [US1] Create FramePointerHealth dataclass with confidence property in `src/spprof/memprof.py`
- [x] T081 [US1] Create MemProfStats dataclass in `src/spprof/memprof.py`
- [x] T082 [US1] Implement start() Python function in `src/spprof/memprof.py`
- [x] T083 [US1] Implement stop() Python function in `src/spprof/memprof.py`
- [x] T084 [US1] Implement get_snapshot() Python function in `src/spprof/memprof.py`
- [x] T085 [US1] Implement get_stats() Python function in `src/spprof/memprof.py`
- [x] T086 [US1] Implement shutdown() Python function in `src/spprof/memprof.py`

### Symbol Resolution

- [x] T087 [US2] Implement resolve_mixed_stack() using existing resolver.c in `src/spprof/_ext/memprof/stack_capture.c`
- [x] T088 [US2] Implement memprof_resolve_symbols() for stack table in `src/spprof/_ext/memprof/memprof.c`
- [x] T089 [US2] Integrate symbol resolution into get_snapshot() path in `src/spprof/_ext/module.c`

### Type Stubs

- [x] T090 [US1] Add memprof type stubs to `src/spprof/_profiler.pyi`

**Checkpoint**: User Stories 1-3 complete - basic profiling works with NumPy visibility

---

## Phase 5: User Stories 4-6 (P2) - Enhanced API

**Goal**: Context manager, combined profiling, export formats

### User Story 4 - Context Manager

**Independent Test**: Profile code block with `with` statement, verify only block allocations captured

- [x] T091 [US4] Implement MemoryProfiler context manager class in `src/spprof/memprof.py`
- [x] T092 [US4] Test context manager scoped profiling in `tests/test_memprof.py`

### User Story 5 - Combined CPU + Memory Profiling

**Independent Test**: Run both profilers simultaneously, verify no interference

- [x] T093 [US5] Verify CPU and memory profilers can run simultaneously in `tests/test_memprof.py`
- [x] T094 [US5] Document combined profiling in examples in `examples/combined_profile.py`

### User Story 6 - Snapshot Export

**Independent Test**: Export snapshot to Speedscope JSON, verify file loads in speedscope.app

- [x] T095 [US6] Implement HeapSnapshot.save() for Speedscope format in `src/spprof/memprof.py`
- [x] T096 [US6] Implement HeapSnapshot.save() for collapsed format in `src/spprof/memprof.py`
- [x] T097 [US6] Reuse existing output.py formatting infrastructure in `src/spprof/memprof.py`
- [x] T098 [US6] Test Speedscope output compatibility in `tests/test_memprof.py`

**Checkpoint**: User Stories 4-6 complete - context manager and export work

---

## Phase 6: User Stories 7-8 (P3) - Advanced Features

**Goal**: Allocation lifetime tracking, profiler diagnostics

### User Story 7 - Allocation Lifetime Tracking

**Independent Test**: Allocate/free objects, verify freed allocations show lifetime duration

- [x] T099 [US7] Implement lifetime duration calculation in heap_map_remove() in `src/spprof/_ext/memprof/heap_map.c`
- [x] T100 [US7] Expose lifetime_ns in AllocationSample in `src/spprof/memprof.py`
- [x] T101 [US7] Test lifetime tracking for freed allocations in `tests/test_memprof.py`

### User Story 8 - Profiler Statistics and Diagnostics

**Independent Test**: Get stats, verify sample counts, heap estimate, load factor reported

- [x] T102 [US8] Implement heap_map_load_percent exposure in stats in `src/spprof/_ext/memprof/memprof.c`
- [x] T103 [US8] Add collision counters to MemProfStats in `src/spprof/memprof.py`
- [x] T104 [US8] Test statistics accuracy in `tests/test_memprof.py`

**Checkpoint**: User Stories 7-8 complete - all features implemented

---

## Phase 7: Production Hardening

**Purpose**: Fork safety, long-running stability, documentation

### Fork Safety

- [x] T105 Implement pthread_atfork handlers (prefork, postfork_parent, postfork_child) in `src/spprof/_ext/memprof/memprof.c`
- [x] T106 Implement PID-based fork detection for vfork safety in `src/spprof/_ext/memprof/sampling.c`
- [x] T107 Test fork safety with multiprocessing in `tests/test_memprof_safety.py`

### Bloom Filter Saturation Handling

- [x] T108 Implement bloom_needs_rebuild() saturation check in `src/spprof/_ext/memprof/bloom.c`
- [x] T109 Integrate bloom rebuild trigger into sampling cold path in `src/spprof/_ext/memprof/sampling.c`

### Safety Tests

- [x] T110 [P] Test re-entrancy safety (allocations in profiler code) in `tests/test_memprof_safety.py`
- [x] T111 [P] Test graceful degradation on heap map overflow in `tests/test_memprof_safety.py`
- [x] T112 [P] Test graceful degradation on stack table overflow in `tests/test_memprof_safety.py`
- [ ] T113 AddressSanitizer (ASan) CI configuration in `.github/workflows/`

---

## Phase 8: Polish & Cross-Cutting Concerns

**Purpose**: Documentation, examples, final cleanup

- [x] T114 [P] Create basic_profile.py example in `examples/`
- [x] T115 [P] Create production_profile.py example in `examples/`
- [x] T116 [P] Update README.md with memory profiler documentation
- [x] T117 [P] Add memory profiler section to docs/USAGE.md
- [x] T118 Run quickstart.md validation scenarios
- [x] T119 Performance benchmark at various sampling rates in `benchmarks/memory.py`
- [x] T120 Memory footprint verification (<60MB) in `benchmarks/memory.py`
- [x] T121 Final code review and cleanup

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies - can start immediately
- **Foundational (Phase 2)**: Depends on Setup completion - **BLOCKS all user stories**
- **Platform (Phase 3)**: Depends on Foundational - enables first tests on real workloads
- **User Stories 1-3 (Phase 4)**: Depends on Phase 3 - this is the **MVP**
- **User Stories 4-6 (Phase 5)**: Depends on Phase 4
- **User Stories 7-8 (Phase 6)**: Depends on Phase 4
- **Hardening (Phase 7)**: Depends on Phase 4, can parallel with Phase 5-6
- **Polish (Phase 8)**: Depends on all feature phases

### User Story Dependencies

| Story | Priority | Depends On | Notes |
|-------|----------|------------|-------|
| US1-3 | P1 | Foundational + Platform | Core MVP - all required together |
| US4 | P2 | US1 | Context manager wraps core API |
| US5 | P2 | US1 | Tests independence from CPU profiler |
| US6 | P2 | US1 | Export uses HeapSnapshot |
| US7 | P3 | US1 | Lifetime data already captured |
| US8 | P3 | US1 | Stats already collected |

### Within Each Phase

- Headers before implementations
- Data structures before algorithms
- Core API before Python bindings
- Implementation before tests
- Tests must FAIL before implementation passes them

### Parallel Opportunities

```
Phase 1 (Setup):
  T002, T003, T004, T005, T006, T007 all [P] - different header files

Phase 2.1-2.6 (Foundational):
  Most tasks sequential within subsystem
  Different subsystems can parallelize after their headers exist

Phase 3 (Platform):
  T056 (macOS test) [P] with T063 (Linux test) - different platforms

Phase 4 (Tests):
  T065, T066, T067, T068, T069 all [P] - different test files/focuses

Phase 4 (Python):
  T077-T081 all dataclasses, can parallel
  T082-T086 all functions, can parallel after dataclasses

Phase 7-8 (Hardening/Polish):
  T110, T111, T112 safety tests [P]
  T114, T115, T116, T117 documentation [P]
```

---

## Parallel Example: Phase 2 Data Structures

```bash
# After headers exist (T002-T007), these subsystems can parallelize:

# Subsystem 1: Heap Map (T009-T015)
# Subsystem 2: Bloom Filter (T016-T021)
# Subsystem 3: Stack Intern (T022-T026)
# Subsystem 4: Sampling Engine (T027-T032)

# Then stack capture (T033-T038) needs sampling engine complete
# Then core lifecycle (T039-T046) orchestrates everything
```

---

## Implementation Strategy

### MVP First (User Stories 1-3)

1. Complete Phase 1: Setup
2. Complete Phase 2: Foundational (CRITICAL - core C infrastructure)
3. Complete Phase 3: Platform (at least macOS)
4. Complete Phase 4: User Stories 1-3
5. **STOP and VALIDATE**: 
   - `memprof.start()` / `stop()` / `get_snapshot()` work
   - NumPy allocations captured
   - Overhead < 0.1% at default rate
6. Deploy/demo if ready

### Incremental Delivery

1. Setup + Foundational + Platform → Infrastructure ready
2. Add US1-3 → Test independently → **MVP Ready!**
3. Add US4-6 → Context manager, export formats → **Enhanced API**
4. Add US7-8 → Lifetime tracking, diagnostics → **Full Feature Set**
5. Hardening + Polish → **Production Ready**

### Critical Path

```
T001 → T002-T007 → T009-T046 → T052-T064 → T070-T090 → MVP Complete
       (headers)   (C core)    (platform)   (Python)
```

The critical path is approximately:
- 8 setup tasks
- 38 foundational tasks  
- 13 platform tasks
- 21 Python API tasks
- **= ~80 tasks to MVP**

---

## Task Summary

| Phase | Tasks | Parallel | Description |
|-------|-------|----------|-------------|
| 1. Setup | T001-T008 | 6 | Headers and structure |
| 2. Foundational | T009-T051 | 5 | Core C infrastructure |
| 3. Platform | T052-T064 | 1 | macOS + Linux hooks |
| 4. US1-3 (P1) | T065-T090 | 5 | Core profiling MVP |
| 5. US4-6 (P2) | T091-T098 | 0 | Enhanced API |
| 6. US7-8 (P3) | T099-T104 | 0 | Advanced features |
| 7. Hardening | T105-T113 | 3 | Production safety |
| 8. Polish | T114-T121 | 4 | Docs and cleanup |
| **Total** | **121** | **24** | |

---

## Notes

- [P] tasks = different files, no dependencies on in-progress tasks
- [US?] label maps task to specific user story
- US1-3 are tightly coupled and form the MVP together
- Foundational phase is large but necessary - it's the core C infrastructure
- Platform phase can start with macOS (simpler) while Linux is developed
- Each user story should be independently testable after US1-3 complete
- Commit after each task or logical group
- Run ASan/TSan in CI for memory safety verification

