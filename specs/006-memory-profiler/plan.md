# Implementation Plan: Memory Allocation Profiler

**Branch**: `006-memory-profiler` | **Date**: December 3, 2024 | **Spec**: [spec.md](spec.md)  
**Input**: Feature specification from `/specs/006-memory-profiler/spec.md`

---

## Summary

Build a production-grade memory allocation profiler for Python that uses **Poisson sampling via native allocator interposition** to provide statistically accurate heap profiling with ultra-low overhead (<0.1% CPU). The implementation captures allocations from Python code, C extensions, and native libraries, producing Speedscope-compatible output for visualization.

**Key Technical Approach**:
- Poisson sampling with exponential inter-sample intervals for unbiased heap estimation
- Platform-native interposition (LD_PRELOAD on Linux, malloc_logger on macOS)
- Lock-free heap map with two-phase insert (reserve→finalize)
- Bloom filter for fast-path free() rejection (~3ns vs ~15ns)
- Mixed-mode stack capture (Python + native frames via existing framewalker)
- Synchronous symbol resolution on stop/snapshot to avoid dl* lock contention

---

## Technical Context

**Language/Version**: Python 3.9–3.14, C17 (extension)  
**Primary Dependencies**: None beyond Python stdlib (reuses existing spprof C infrastructure)  
**Storage**: N/A (in-memory data structures, file output via snapshot.save())  
**Testing**: pytest, AddressSanitizer, custom concurrent stress tests  
**Target Platform**: Linux (primary), macOS, Windows (experimental)  
**Project Type**: Single project (Python package with C extension)  
**Performance Goals**: < 0.1% CPU overhead at 512KB sampling rate, < 10 cycles hot path  
**Constraints**: ≤ 60 MB memory footprint, lock-free hot path, re-entrancy safe  
**Scale/Scope**: Single-process profiling, 1–100 threads, weeks of continuous operation

---

## Constitution Check

*GATE: Verified against `.specify/memory/constitution.md`*

| Principle | Compliance | Notes |
|-----------|------------|-------|
| **Minimal Overhead** | ✅ PASS | Poisson sampling + Bloom filter keeps hot path < 10 cycles |
| **Memory Safety** | ✅ PASS | Lock-free CAS operations; no malloc in hot path; re-entrancy guard |
| **Cross-Platform** | ✅ PASS | Platform abstraction: LD_PRELOAD (Linux), malloc_logger (macOS) |
| **Statistical Accuracy** | ✅ PASS | Unbiased Poisson sampling; error bounds documented |
| **Clean C-Python Boundary** | ✅ PASS | C handles sampling/storage; Python handles API/formatting |

### Technical Constraints Compliance

| Constraint | Compliance | Notes |
|------------|------------|-------|
| Python 3.9–3.14 support | ✅ PASS | Reuses existing framewalker with version dispatch |
| Build system: meson | ✅ PASS | Extends existing meson.build |
| Pre-built wheels | ✅ PASS | CI builds for manylinux, macOS (Windows experimental) |
| Independent from CPU profiler | ✅ PASS | Separate module, can run simultaneously |

**Gate Status**: ✅ PASS - No violations requiring justification

---

## Project Structure

### Documentation (this feature)

```text
specs/006-memory-profiler/
├── plan.md              # This file
├── spec.md              # Feature specification
├── research.md          # Technical decisions (Phase 0)
├── data-model.md        # Entity definitions (Phase 1)
├── quickstart.md        # Usage guide (Phase 1)
├── contracts/
│   ├── python-api.md    # Public Python API contract
│   └── c-internal-api.md # Internal C API contract
├── checklists/
│   └── requirements.md  # Spec quality checklist
└── tasks.md             # Implementation tasks (Phase 2)
```

### Source Code (repository root)

```text
src/spprof/
├── __init__.py              # Existing: CPU profiler
├── memprof.py               # NEW: Python wrapper for memory profiler
├── _profiler.pyi            # UPDATE: Add memprof type stubs
└── _ext/
    ├── module.c             # UPDATE: Add memprof Python bindings
    ├── memprof/             # NEW: Memory profiler C implementation
    │   ├── memprof.h        # Core types and constants
    │   ├── memprof.c        # Lifecycle: init, start, stop, shutdown
    │   ├── heap_map.c       # Lock-free heap map implementation
    │   ├── heap_map.h
    │   ├── stack_intern.c   # Stack deduplication table
    │   ├── stack_intern.h
    │   ├── bloom.c          # Bloom filter for free() optimization
    │   ├── bloom.h
    │   ├── sampling.c       # PRNG, threshold generation, TLS
    │   ├── sampling.h
    │   ├── stack_capture.c  # Native + mixed-mode stack capture
    │   └── stack_capture.h
    ├── platform/
    │   ├── linux_memprof.c  # NEW: LD_PRELOAD interposition
    │   ├── darwin_memprof.c # NEW: malloc_logger callback
    │   └── windows_memprof.c # NEW: Detours hooks (experimental)
    ├── framewalker.c        # REUSE: Python frame walking
    └── resolver.c           # REUSE: Symbol resolution

tests/
├── test_memprof.py               # NEW: Integration tests
├── test_memprof_data_structures.py # NEW: Heap map, stack table, bloom, PRNG unit tests
├── test_memprof_stress.py        # NEW: Concurrent stress tests
└── test_memprof_safety.py        # NEW: Re-entrancy, overflow tests

benchmarks/
└── memory.py                # EXISTING: Extend with memprof benchmarks
```

**Structure Decision**: Extend existing spprof structure. Memory profiler lives alongside CPU profiler in `_ext/` with its own subdirectory. Reuses framewalker.c and resolver.c. Platform hooks in `platform/` directory.

---

## Architecture Overview

```
┌────────────────────────────────────────────────────────────────────────────┐
│                           Python Application                                │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │  import spprof.memprof as memprof                                     │  │
│  │  memprof.start(sampling_rate_kb=512)                                  │  │
│  │  # ... allocate memory ...                                            │  │
│  │  snapshot = memprof.get_snapshot()                                    │  │
│  │  snapshot.save("heap.json")                                           │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
└────────────────────────────────────────────────────────────────────────────┘
                                     │
                                     ▼
┌────────────────────────────────────────────────────────────────────────────┐
│                         spprof.memprof Module                               │
│  ┌─────────────────┐  ┌─────────────────┐  ┌─────────────────────────┐    │
│  │  memprof.py     │  │  output.py      │  │  _profiler.pyi         │    │
│  │  (Python API)   │  │  (formatters)   │  │  (type stubs)          │    │
│  └─────────────────┘  └─────────────────┘  └─────────────────────────┘    │
└────────────────────────────────────────────────────────────────────────────┘
                                     │
                                     ▼
┌────────────────────────────────────────────────────────────────────────────┐
│                    spprof._native (C Extension)                             │
│                                                                             │
│  ┌───────────────────────────────────────────────────────────────────┐    │
│  │                     module.c (Python bindings)                     │    │
│  └───────────────────────────────────────────────────────────────────┘    │
│                                     │                                      │
│  ┌─────────────────────────────────────────────────────────────────┐      │
│  │                      memprof/ subsystem                          │      │
│  │                                                                  │      │
│  │  ┌──────────┐    ┌──────────┐    ┌──────────┐    ┌──────────┐  │      │
│  │  │memprof.c │    │heap_map.c│    │stack_    │    │ bloom.c  │  │      │
│  │  │(lifecycle)│    │(lock-free│    │intern.c  │    │(filter)  │  │      │
│  │  └──────────┘    │hash table)│    │(dedup)   │    └──────────┘  │      │
│  │                  └──────────┘    └──────────┘                   │      │
│  │                                                                  │      │
│  │  ┌──────────────────────────────────────────────────────────┐   │      │
│  │  │                    sampling.c                             │   │      │
│  │  │  (TLS, PRNG, threshold generation, hot/cold path)        │   │      │
│  │  └──────────────────────────────────────────────────────────┘   │      │
│  │                          │                                       │      │
│  │                          ▼                                       │      │
│  │  ┌──────────────────────────────────────────────────────────┐   │      │
│  │  │                  stack_capture.c                          │   │      │
│  │  │  (frame pointer walking + framewalker.c integration)     │   │      │
│  │  └──────────────────────────────────────────────────────────┘   │      │
│  └─────────────────────────────────────────────────────────────────┘      │
│                                     │                                      │
│  ┌─────────────────────────────────────────────────────────────────┐      │
│  │                     platform/ interposition                      │      │
│  │  linux_memprof.c  │  darwin_memprof.c  │  windows_memprof.c    │      │
│  │  (LD_PRELOAD)     │  (malloc_logger)   │  (Detours)            │      │
│  └─────────────────────────────────────────────────────────────────┘      │
└────────────────────────────────────────────────────────────────────────────┘
```

---

## Non-Functional Requirements (NFRs)

### Performance

| ID | Requirement | Target | Verification |
|----|-------------|--------|--------------|
| NFR-001 | CPU overhead @ 512KB rate | < 0.1% | Benchmark: CPU time with/without profiler |
| NFR-002 | CPU overhead @ 64KB rate | < 1% | Benchmark: CPU time with/without profiler |
| NFR-003 | Hot path cycles | < 10 cycles | Measurement: TLS access + subtract + branch |
| NFR-004 | Cold path latency | < 1μs | Measurement: stack capture + insert |
| NFR-005 | Free path (non-sampled) | < 5ns | Measurement: Bloom filter check |
| NFR-006 | Free path (sampled) | < 30ns | Measurement: hash + delete |

### Memory

| ID | Requirement | Target | Verification |
|----|-------------|--------|--------------|
| NFR-007 | Heap map memory | 24 MB (fixed) | 1M entries × 24 bytes |
| NFR-008 | Stack table (initial) | ~2 MB | 4K entries × 544 bytes |
| NFR-009 | Stack table (max) | ~35 MB | 64K entries × 544 bytes |
| NFR-010 | Bloom filter | 128 KB | 1M bits |
| NFR-011 | Total footprint | ≤ 60 MB | Sum of above + TLS |
| NFR-012 | No memory leaks | 0 leaks | ASan in CI |

### Reliability

| ID | Requirement | Target | Verification |
|----|-------------|--------|--------------|
| NFR-013 | Lock-free hot path | No locks | Code review + stress test |
| NFR-014 | Re-entrancy safe | No recursion | Guard check in all hooks |
| NFR-015 | Concurrent safety | No data races | ThreadSanitizer in CI |
| NFR-016 | Graceful degradation | Drop samples, don't crash | Overflow stress test |
| NFR-017 | Long-running stability | Weeks of operation | Soak test |

### Accuracy

| ID | Requirement | Target | Verification |
|----|-------------|--------|--------------|
| NFR-018 | Heap estimate accuracy | ±20% with 95% CI | Statistical validation |
| NFR-019 | Allocation attribution | Correct call stacks | Integration tests |
| NFR-020 | Python frame resolution | Function, file, line | Output validation |

---

## Implementation Phases

> **Note**: These design phases describe logical groupings. The `tasks.md` reorganizes these into an optimized implementation order where symbol resolution (Phase 7 here) is integrated into User Story 2-3 tasks for better task flow.

### Phase 1: Core Data Structures

**Goal**: Lock-free heap map and stack intern table

1. Heap map with two-phase insert (reserve/finalize)
2. State machine: EMPTY → RESERVED → ptr → TOMBSTONE
3. Stack intern table with FNV-1a hashing
4. Bloom filter for free() optimization
5. Unit tests for concurrent operations

**Deliverables**:
- `heap_map.c`, `stack_intern.c`, `bloom.c`
- Unit tests with concurrent stress
- Memory safety verified via ASan

### Phase 2: Sampling Engine

**Goal**: Poisson sampling with per-thread TLS

1. xorshift128+ PRNG implementation
2. Exponential threshold generation
3. TLS state management (byte_counter, PRNG state)
4. Re-entrancy guard
5. Hot path optimization (< 10 cycles)

**Deliverables**:
- `sampling.c`, `sampling.h`
- Hot path benchmark
- TLS initialization tests

### Phase 3: Stack Capture

**Goal**: Mixed-mode Python + native stack capture

1. Native frame pointer walking (architecture-specific)
2. Integration with existing framewalker.c
3. "Trim & Sandwich" merge algorithm
4. Frame pointer health tracking

**Deliverables**:
- `stack_capture.c`
- Mixed-mode stack tests
- Frame pointer warning system

### Phase 4: Platform Interposition (macOS First)

**Goal**: malloc_logger callback on macOS

1. malloc_logger callback installation
2. Sequence counter for ABA detection
3. Thread-safe install/remove
4. Integration with sampling engine

**Deliverables**:
- `darwin_memprof.c`
- macOS integration tests
- Zombie race detection tests

### Phase 5: Platform Interposition (Linux)

**Goal**: LD_PRELOAD library on Linux

1. dlsym(RTLD_NEXT) for real malloc/free
2. Bootstrap heap for init-time allocations
3. Fail-fast on dlsym failure
4. Build system for shared library

**Deliverables**:
- `linux_memprof.c`
- `libspprof_alloc.so` build
- Linux integration tests

### Phase 6: Python API

**Goal**: Complete Python module

1. memprof.py wrapper (start, stop, get_snapshot, get_stats, shutdown)
2. Data classes (AllocationSample, HeapSnapshot, MemProfStats)
3. Context manager (MemoryProfiler)
4. Speedscope output format
5. Type stubs (_profiler.pyi)

**Deliverables**:
- `memprof.py`
- Updated `_profiler.pyi`
- Python API tests

### Phase 7: Symbol Resolution

**Goal**: Resolve addresses to function/file/line

1. Integrate with existing resolver.c
2. Synchronous resolution on stop/get_snapshot
3. dladdr for native symbols
4. Python code object resolution

**Deliverables**:
- Resolution integration
- Output format tests
- Speedscope compatibility verified

### Phase 8: Production Hardening

**Goal**: Production-ready reliability

1. Bloom filter saturation monitoring and rebuild
2. Fork safety (pthread_atfork handlers)
3. Long-running soak tests
4. Documentation and examples

**Deliverables**:
- Fork safety tests
- Soak test passing (24+ hours)
- Documentation complete
- Example scripts

---

## Risk Register

| Risk | Impact | Probability | Mitigation |
|------|--------|-------------|------------|
| malloc_logger ABA race | High | Medium | Sequence counter with deterministic detection |
| dlsym recursion on Linux | Critical | Medium | Bootstrap heap + init guard |
| Frame pointers missing in C extensions | Medium | High | Runtime warning + DWARF fallback option |
| Bloom filter saturation in long runs | Medium | Low | Background rebuild + saturation monitoring |
| Stack table capacity exceeded | Medium | Low | Dynamic growth + drop with warning |
| Lock contention with dlopen | Medium | Low | Synchronous resolution (no background thread) |

---

## Testing Strategy

### Unit Tests
- Heap map concurrent insert/remove
- Stack intern deduplication
- Bloom filter false positive rate
- PRNG statistical properties
- Exponential distribution validation

### Integration Tests
- Full profiling cycle (start → workload → snapshot → stop)
- NumPy/PyTorch allocation capture
- Context manager API
- Combined CPU + memory profiling
- Output format validation

### Safety Tests
- Re-entrancy stress (allocations in profiler code)
- High allocation rate (1M+ allocs/sec)
- Concurrent allocation from 10+ threads
- Fork during profiling
- Overflow handling (heap map full)

### Platform Tests
- macOS malloc_logger
- Linux LD_PRELOAD
- Python 3.9–3.14 matrix
- ASan/TSan in CI

### Performance Tests
- Hot path cycle count
- Free path latency (Bloom filter)
- Cold path latency (sampling)
- Memory footprint verification
- Overhead at various sampling rates

---

## Artifacts Generated

| Artifact | Path | Purpose |
|----------|------|---------|
| Research | [research.md](research.md) | Technical decisions |
| Data Model | [data-model.md](data-model.md) | Entity definitions |
| Python API | [contracts/python-api.md](contracts/python-api.md) | Public API contract |
| C API | [contracts/c-internal-api.md](contracts/c-internal-api.md) | Internal API contract |
| Quickstart | [quickstart.md](quickstart.md) | Usage guide |

---

## Next Steps

1. **`/speckit.tasks`** — Break this plan into actionable implementation tasks
2. **`/speckit.checklist`** — Create implementation quality checklist
3. Begin Phase 1 with heap map and stack intern table implementation
