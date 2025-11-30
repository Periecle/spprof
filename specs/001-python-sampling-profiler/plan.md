# Implementation Plan: Python Sampling Profiler

**Branch**: `001-python-sampling-profiler` | **Date**: 2025-11-29 | **Spec**: [spec.md](spec.md)  
**Input**: Feature specification from `/specs/001-python-sampling-profiler/spec.md`

---

## Summary

Build a high-performance, in-process sampling profiler for Python that uses SIGPROF-based signal interrupts for low-overhead CPU profiling. The implementation must support Python 3.9–3.14 despite significant CPython internal API changes, operate in restricted Kubernetes environments (no SYS_PTRACE, no CAP_PERFMON), and produce Speedscope-compatible output for flame graph visualization.

**Key Technical Approach**: 
- Pre-allocated ring buffer for async-signal-safe sample capture
- Compile-time version dispatch for CPython frame structure polymorphism
- Lazy symbol resolution in background thread with GIL acquisition
- Platform-specific timer implementations (timer_create on Linux, setitimer on macOS, suspend-and-sample on Windows)

---

## Technical Context

**Language/Version**: Python 3.9–3.14, C17 (extension)  
**Primary Dependencies**: None (zero runtime dependencies beyond Python stdlib)  
**Storage**: N/A (in-memory ring buffer, file output on stop)  
**Testing**: pytest, AddressSanitizer, custom deadlock detection  
**Target Platform**: Linux (primary), macOS, Windows  
**Project Type**: Single project (Python package with C extension)  
**Performance Goals**: < 1% CPU overhead at 10ms sampling, < 10μs signal handler execution  
**Constraints**: ≤ 100MB memory footprint (configurable), async-signal-safe, no GIL in hot path  
**Scale/Scope**: Single-process profiling, 1–100 threads, 10K–1M samples/session

---

## Constitution Check

*GATE: Verified against `.specify/memory/constitution.md`*

| Principle | Compliance | Notes |
|-----------|------------|-------|
| **I. Minimal Overhead** | ✅ PASS | Ring buffer + deferred resolution keeps handler < 10μs |
| **II. Memory Safety** | ✅ PASS | No malloc in signal handler; reference counting in safe context only |
| **III. Cross-Platform** | ✅ PASS | Platform abstraction layer for Linux/macOS/Windows |
| **IV. Statistical Accuracy** | ✅ PASS | Wall-clock sampling, full stack capture, thread attribution |
| **V. Clean C-Python Boundary** | ✅ PASS | C handles sampling; Python handles formatting/API |

### Technical Constraints Compliance

| Constraint | Compliance | Notes |
|------------|------------|-------|
| Python 3.9–3.14 support | ✅ PASS | Compile-time version dispatch |
| Stable ABI (Py_LIMITED_API) | ⚠️ PARTIAL | Cannot use stable ABI for frame access (internal structs) |
| Build system: pyproject.toml + uv | ✅ PASS | Standard Python packaging |
| Pre-built wheels | ✅ PASS | CI builds for manylinux, macOS, Windows |

**Gate Status**: ✅ PASS - No violations requiring justification

---

## Project Structure

### Documentation (this feature)

```text
specs/001-python-sampling-profiler/
├── plan.md              # This file
├── spec.md              # Feature specification
├── research.md          # Technical decisions (Phase 0)
├── data-model.md        # Entity definitions (Phase 1)
├── quickstart.md        # Usage guide (Phase 1)
├── contracts/
│   ├── python-api.md    # Public Python API contract
│   └── c-internal-api.md # Internal C API contract
└── checklists/
    └── requirements.md  # Spec quality checklist
```

### Source Code (repository root)

```text
spprof/
├── pyproject.toml              # Build config (uv/hatchling)
├── README.md                   # Package documentation
├── src/
│   └── spprof/
│       ├── __init__.py         # Public Python API
│       ├── _profiler.pyi       # Type stubs for C extension
│       ├── output.py           # Output formatters (Speedscope, collapsed)
│       └── _native/
│           ├── module.c        # Python module definition
│           ├── sampler.c       # Signal handler, timer setup
│           ├── ringbuffer.c    # Lock-free ring buffer
│           ├── ringbuffer.h
│           ├── framewalker.c   # Version-polymorphic frame walking
│           ├── framewalker.h
│           ├── resolver.c      # Symbol resolution (consumer)
│           ├── resolver.h
│           ├── unwind.c        # Optional: libunwind C-stack capture (Linux)
│           ├── unwind.h
│           ├── platform/
│           │   ├── platform.h  # Platform abstraction
│           │   ├── linux.c     # Linux: timer_create + SIGPROF
│           │   ├── darwin.c    # macOS: setitimer + SIGPROF
│           │   └── windows.c   # Windows: CreateTimerQueueTimer
│           └── compat/
│               ├── py39.h      # Python 3.9-3.10 frame structures
│               ├── py311.h     # Python 3.11 _PyInterpreterFrame
│               ├── py312.h     # Python 3.12 tagged pointers
│               ├── py313.h     # Python 3.13 free-threading
│               └── py314.h     # Python 3.14 tail-call compat
├── tests/
│   ├── test_profiler.py        # Integration tests
│   ├── test_output.py          # Output format tests
│   ├── test_threading.py       # Multi-thread tests
│   └── test_signal_safety.py   # Deadlock detection tests
└── benchmarks/
    └── overhead.py             # CPU overhead measurement
```

**Structure Decision**: Single project with C extension. Python package provides public API; C extension provides low-level sampling. Platform and version-specific code isolated in subdirectories.

---

## Architecture Overview

```
┌────────────────────────────────────────────────────────────────────────┐
│                           Python Application                            │
│  ┌──────────────────────────────────────────────────────────────────┐  │
│  │  import spprof                                                    │  │
│  │  spprof.start(interval_ms=10)                                     │  │
│  │  # ... workload ...                                               │  │
│  │  profile = spprof.stop()                                          │  │
│  │  profile.save("profile.json")                                     │  │
│  └──────────────────────────────────────────────────────────────────┘  │
└────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
┌────────────────────────────────────────────────────────────────────────┐
│                        spprof Python Module                             │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────────────┐    │
│  │ __init__.py │  │ output.py   │  │ _profiler.pyi (type stubs) │    │
│  │ (public API)│  │ (formatters)│  └─────────────────────────────┘    │
│  └─────────────┘  └─────────────┘                                      │
└────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
┌────────────────────────────────────────────────────────────────────────┐
│                      spprof._native (C Extension)                       │
│                                                                         │
│  ┌─────────────────────────────────────────────────────────────────┐   │
│  │                         module.c                                 │   │
│  │                    (Python bindings)                             │   │
│  └─────────────────────────────────────────────────────────────────┘   │
│           │                    │                     │                  │
│           ▼                    ▼                     ▼                  │
│  ┌─────────────┐    ┌──────────────────┐    ┌─────────────────┐        │
│  │ sampler.c   │    │  ringbuffer.c    │    │   resolver.c    │        │
│  │ (signals)   │───►│  (lock-free Q)   │───►│ (symbols)       │        │
│  └─────────────┘    └──────────────────┘    └─────────────────┘        │
│           │                                          │                  │
│           ▼                                          │                  │
│  ┌─────────────────────────────────────────┐         │                  │
│  │           framewalker.c                  │         │                  │
│  │  (version-polymorphic stack walking)     │◄────────┘                  │
│  └─────────────────────────────────────────┘                            │
│           │                                                             │
│           ▼                                                             │
│  ┌─────────────────────────────────────────────────────────────────┐   │
│  │                     compat/ headers                              │   │
│  │  py39.h │ py311.h │ py312.h │ py313.h │ py314.h                  │   │
│  └─────────────────────────────────────────────────────────────────┘   │
│           │                                                             │
│           ▼                                                             │
│  ┌─────────────────────────────────────────────────────────────────┐   │
│  │                    platform/ implementations                     │   │
│  │  linux.c (timer_create) │ darwin.c (setitimer) │ windows.c      │   │
│  └─────────────────────────────────────────────────────────────────┘   │
└────────────────────────────────────────────────────────────────────────┘
```

---

## Non-Functional Requirements (NFRs)

### Performance

| ID | Requirement | Target | Verification |
|----|-------------|--------|--------------|
| NFR-001 | CPU overhead @ 10ms interval | < 1% | Benchmark: compare CPU time with/without profiler |
| NFR-002 | CPU overhead @ 1ms interval | < 5% | Benchmark: compare CPU time with/without profiler |
| NFR-003 | Signal handler execution time | < 10μs | Measurement: rdtsc before/after handler |
| NFR-004 | Profiler start latency | < 50ms | Measurement: time from start() call to first sample |
| NFR-005 | Profiler stop latency | < 100ms | Measurement: time from stop() call to profile return |
| NFR-006 | Symbol resolution throughput | > 100K frames/sec | Benchmark: time to resolve sample batch |

### Memory

| ID | Requirement | Target | Verification |
|----|-------------|--------|--------------|
| NFR-007 | Ring buffer memory | ≤ 16MB | Fixed: 65536 slots × 256 bytes |
| NFR-008 | Symbol cache memory | ≤ 32MB | Bounded LRU cache |
| NFR-009 | Total memory footprint | ≤ 100MB (configurable) | Peak memory measurement |
| NFR-010 | No memory leaks | 0 leaks | Valgrind / ASan in CI |

### Reliability

| ID | Requirement | Target | Verification |
|----|-------------|--------|--------------|
| NFR-011 | No deadlocks in signal handler | 0 occurrences | Stress test with watchdog |
| NFR-012 | No crashes from frame walking | 0 occurrences | Fuzz test with GC stress |
| NFR-013 | Graceful overflow handling | Drop, don't crash | Overflow stress test |
| NFR-014 | Process survives profiler failure | 100% | Fault injection test |

### Compatibility

| ID | Requirement | Target | Verification |
|----|-------------|--------|--------------|
| NFR-015 | Python version coverage | 3.9–3.14 | CI matrix |
| NFR-016 | Platform coverage | Linux, macOS, Windows | CI matrix |
| NFR-017 | Free-threaded Python (3.13+) | Functional | CI with --disable-gil build |
| NFR-018 | Restricted K8s operation | Functional | K8s integration test |
| NFR-019 | Non-root execution | Functional | CI runs as non-root |

---

## Implementation Phases

### Phase 1: Core Infrastructure

**Goal**: Minimal working profiler on Linux with single Python version

1. Ring buffer implementation (C)
2. Basic signal handler with frame pointer capture
3. Python module skeleton with start/stop API
4. Speedscope JSON output

**Deliverables**: 
- Working profiler for Python 3.12 on Linux
- Basic test suite
- Single-threaded profiling

### Phase 2: Version Polymorphism

**Goal**: Support all Python versions 3.9–3.14

1. Frame walker abstraction layer
2. Version-specific compat headers
3. Tagged pointer handling (3.12+)
4. Free-threaded support (3.13+)
5. Tail-call interpreter verification (3.14)

**Deliverables**:
- CI matrix covering all Python versions
- Version-specific tests

### Phase 3: Multi-Threading & Platforms

**Goal**: Full thread support and cross-platform

1. Per-thread timer setup (Linux timer_create)
2. macOS setitimer implementation
3. Windows suspend-and-sample implementation
4. Thread registration API

**Deliverables**:
- Multi-threaded profiling on all platforms
- CI matrix for Linux, macOS, Windows

### Phase 4: Production Hardening

**Goal**: Production-ready reliability

1. Symbol cache with LRU eviction
2. GC stress testing
3. Deadlock detection tests
4. Memory safety verification (ASan)
5. Overflow handling tests

**Deliverables**:
- Full test suite with safety tests
- Memory safety CI integration
- Performance benchmarks

### Phase 5: Polish & Release

**Goal**: Ready for public use

1. Type stubs and documentation
2. Wheel builds for all platforms
3. PyPI publishing
4. Kubernetes integration tests

**Deliverables**:
- Published package on PyPI
- Complete documentation
- Example applications

---

## Risk Register

| Risk | Impact | Probability | Mitigation |
|------|--------|-------------|------------|
| Python 3.14 frame structure changes | High | Medium | Monitor CPython PRs; have fallback to skip 3.14 initially |
| Deadlock in signal handler | Critical | Low | Strict async-signal-safety; watchdog tests |
| Windows suspend overhead too high | Medium | Medium | Document overhead; recommend Linux for production |
| GC invalidates code object pointers | Medium | Low | Validity check before dereference; skip invalid frames |
| Symbol cache memory explosion | Medium | Low | LRU eviction with hard cap |

---

## Testing Strategy

### Unit Tests
- Ring buffer read/write correctness
- Frame walker version dispatch
- Output format validation

### Integration Tests
- Full profiling cycle (start → workload → stop → output)
- Multi-threaded profiling
- Context manager and decorator APIs

### Safety Tests
- Deadlock detection with watchdog timeout
- GC stress during profiling
- Ring buffer overflow handling
- AddressSanitizer in CI

### Performance Tests
- Overhead measurement at various intervals
- Signal handler timing
- Symbol resolution throughput

### Platform Tests
- CI matrix: Linux, macOS, Windows
- Python version matrix: 3.9–3.14
- Kubernetes integration test

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
3. Begin Phase 1 implementation with ring buffer and signal handler
