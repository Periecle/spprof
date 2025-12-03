# Research: Memory Allocation Profiler

**Feature**: 006-memory-profiler  
**Date**: December 3, 2024

---

## Overview

This document consolidates technical decisions and research for implementing a production-grade memory allocation profiler with Poisson sampling.

---

## R1: Sampling Algorithm

**Decision**: Poisson sampling with exponential inter-sample intervals

**Rationale**:
- Counting every allocation is prohibitively expensive (~3% CPU at 1M allocs/sec)
- Poisson sampling provides **unbiased heap estimation** with bounded error
- Larger allocations are proportionally more likely to be sampled (size-weighted)
- Expected contribution of any allocation: `(size/sampling_rate) × sampling_rate = size` ✓

**Alternatives Rejected**:

| Alternative | Why Rejected |
|-------------|--------------|
| Count every allocation | 3%+ CPU overhead - unacceptable for production |
| Fixed interval sampling | Biased toward allocation patterns, not allocation sizes |
| Reservoir sampling | Doesn't weight by allocation size |
| tracemalloc | Only tracks Python allocations, not C extensions |

**Implementation**:
- Maintain per-thread `byte_counter` (signed int64)
- Decrement counter by allocation size on each malloc
- When counter ≤ 0, trigger sampling cold path
- Generate next threshold via exponential distribution: `-mean × ln(U)`
- Use xorshift128+ PRNG for fast, high-quality random numbers

**Mathematical Properties**:
- Default rate: 512 KB → ~200 samples/sec at 100 MB/s allocation rate
- Unbiased estimator: `Σ(sample_weight)` equals true heap size in expectation
- Relative error: `1/√n × (σ/μ)` where n = sample count
- 1000 samples → ~6% relative error with 95% confidence

---

## R2: Platform Interposition Mechanism

**Decision**: Platform-specific native allocator interposition

| Platform | Mechanism | Implementation |
|----------|-----------|----------------|
| Linux | LD_PRELOAD library | Replace malloc/free symbols via dynamic linking |
| macOS | malloc_logger callback | Official Apple API for allocation tracking |
| Windows | MS Detours (experimental) | Hook CRT allocation functions |

**Rationale**:
- Must capture allocations from **all sources**: Python, C extensions, native libraries
- PyMem hooks only capture Python allocations, missing NumPy/PyTorch/Rust bindings
- Native interposition is the only way to achieve complete coverage

**Alternatives Rejected**:

| Alternative | Why Rejected |
|-------------|--------------|
| PyMem_SetAllocator | Only captures Python allocations |
| GOT patching | Full RELRO makes this unreliable on modern Linux |
| Manual instrumentation | Doesn't capture third-party library allocations |

**Linux LD_PRELOAD Details**:
- Provide `libspprof_alloc.so` that interposes malloc/calloc/realloc/free
- Use `dlsym(RTLD_NEXT, "malloc")` to get real allocator
- Bootstrap heap handles allocations during dlsym initialization (64 KB static buffer)
- Fail-fast on dlsym failure (statically linked binaries not supported)

**macOS malloc_logger Details**:
- Use `malloc_logger` function pointer callback
- Callback receives allocation events after malloc/free complete (post-hook)
- Must handle "Zombie Killer" race where address is reused before callback runs
- Use global sequence counter for deterministic zombie detection

**Windows Detours Details**:
- Experimental support only in v1.0
- Only hooks CRT malloc/free (HeapAlloc, VirtualAlloc not tracked)
- Document limitations clearly

---

## R3: Lock-Free Data Structures

**Decision**: Lock-free hash table for heap map, lock-free stack intern table

**Rationale**:
- Hot path must be <10 cycles for production-safe overhead
- Locks in malloc path cause contention with high thread counts
- CAS operations provide thread safety without blocking

**Heap Map Design**:
- Open-addressing hash table with linear probing
- 1M entries capacity (fixed, ~24 MB memory)
- Key: pointer address; Value: packed metadata (stack_id, size, weight)
- Two-phase insert: RESERVE → FINALIZE (prevents free-before-insert race)
- Tombstone reuse: FREE slots can be reclaimed during insert

**State Machine**:
```
EMPTY → RESERVED (malloc: CAS success)
TOMBSTONE → RESERVED (malloc: CAS success, recycling)
RESERVED → ptr (malloc: finalize)
RESERVED → TOMBSTONE (free: "death during birth")
ptr → TOMBSTONE (free: normal path)
```

**Stack Intern Table Design**:
- Dynamic sizing: 4K initial → 64K max entries
- FNV-1a hash for stack deduplication
- CAS on hash field for claiming empty slots
- Returns uint32_t stack_id for space efficiency

---

## R4: Free Path Optimization (Bloom Filter)

**Decision**: Bloom filter for fast-path free() rejection

**Rationale**:
- 99.99% of frees are for non-sampled allocations
- Without optimization: every free requires hash table probe (~15ns cache miss)
- Bloom filter: O(1) definite-no answer with 0% false negatives

**Parameters**:
- 1M bits = 128 KB (fits in L2 cache)
- 4 hash functions (optimal for expected load)
- ~2% false positive rate at 50K live entries
- Result: 3ns average free path vs 15ns without filter

**Saturation Handling**:
- Long-running processes accumulate stale bits from address reuse
- Monitor saturation via approximate bit count
- Rebuild filter from heap map when >50% saturated
- Intentionally leak old filters (no munmap during operation for safety)
- Cleanup at process exit via leaked filter list

---

## R5: Stack Capture Strategy

**Decision**: Frame pointer walking + mixed-mode Python/native merge

**Rationale**:
- Frame pointer walking is fast (~50-100 cycles) and async-signal-safe
- Users want to see Python function names, not just `PyObject_Call`
- Reuse existing spprof framewalker.c for Python frame capture
- Merge native + Python stacks using "Trim & Sandwich" algorithm

**Mixed-Mode Stack Algorithm**:
1. Capture native frames via frame pointer walking
2. Capture Python frames via framewalker.c (existing infrastructure)
3. During resolution, merge: native leaf → Python frames → native root

**Frame Pointer Limitations**:
- Many C extensions compiled without `-fno-omit-frame-pointer`
- Result: truncated native stacks at that point
- Mitigation: Runtime warning, statistics tracking, documentation

**DWARF Fallback (Optional)**:
- Compile-time flag: `MEMPROF_USE_LIBUNWIND`
- 100-1000× slower than frame pointer walking
- Use for debugging only, not production

---

## R6: Memory Footprint Management

**Decision**: Fixed heap map, dynamic stack table, bounded total footprint

| Component | Initial | Maximum |
|-----------|---------|---------|
| Heap Map | 24 MB | 24 MB (fixed) |
| Stack Table | ~2 MB | ~35 MB (grows on demand) |
| Bloom Filter | 128 KB | 128 KB |
| TLS per thread | 1 KB | 1 KB |
| **Total** | **~27 MB** | **~60 MB** |

**Rationale**:
- Fixed heap map avoids resize complexity during operation
- Dynamic stack table saves memory for simple scripts (~2 MB vs 140 MB)
- Configurable max via `SPPROF_STACK_TABLE_MAX` environment variable

**Stack Table Resize**:
- Grow at 75% load factor
- Linux: mremap() for efficient in-place growth
- macOS/Windows: mmap new + memcpy + munmap old (on background thread)

---

## R7: Concurrency Safety

**Decision**: Strict lock-free hot path, deferred resolution

**Hot Path (99.99% of calls)**:
- TLS access only
- Single atomic decrement
- Branch prediction for fast path

**Cold Path (sampling)**:
- CAS operations for heap map insertion
- Re-entrancy guard prevents infinite recursion
- Bootstrap heap handles initialization allocations

**Thread Safety Guarantees**:
- No locks in malloc/free path
- Packed 64-bit metadata prevents torn reads during snapshot
- Sequence counter prevents ABA problem on macOS post-hook

**Fork Safety**:
- Register pthread_atfork handlers
- Auto-disable profiler in child processes
- PID check detects fork/vfork children

---

## R8: Symbol Resolution Strategy

**Decision**: Synchronous resolution on stop()/get_snapshot(), not background thread

**Rationale**:
- Background resolution causes dl_iterate_phdr lock contention
- Applications using dlopen/dlclose may experience priority inversion
- Synchronous resolution is simpler and avoids all lock issues

**Resolution Timing**:
- Raw PCs stored during sampling (no resolution)
- Resolution happens when stop() or get_snapshot() is called
- Caller can request immediate raw-address snapshot: `get_snapshot(resolve=False)`

**dladdr/DbgHelp Usage**:
- Linux/macOS: dladdr() for native symbol lookup
- Windows: DbgHelp for symbol resolution
- Python frames: Reuse existing resolver.c from CPU profiler

---

## R9: API Design

**Decision**: Mirror CPU profiler API for consistency

**Core API**:
```python
memprof.start(sampling_rate_kb=512)  # Start profiling
memprof.stop()                        # Stop new allocations (frees still tracked)
memprof.get_snapshot()               # Get live allocations
memprof.get_stats()                  # Get profiler statistics
memprof.shutdown()                   # Full shutdown (one-way)
```

**Lifecycle States**:
- UNINITIALIZED → INITIALIZED → ACTIVE → STOPPED → TERMINATED
- stop() disables new allocations but continues tracking frees
- shutdown() is one-way (cannot restart after shutdown)

**Context Manager**:
```python
with memprof.MemoryProfiler(sampling_rate_kb=512) as mp:
    # workload
mp.snapshot.save("profile.json")
```

---

## R10: Output Format

**Decision**: Speedscope-compatible JSON, same as CPU profiler

**Rationale**:
- Consistent tooling across CPU and memory profiling
- Speedscope is widely used and well-maintained
- Collapsed format supported for FlameGraph compatibility

**Snapshot Contents**:
- Live allocation samples with stack traces
- Estimated heap size
- Per-stack aggregated byte counts
- Frame pointer health metrics

---

## Summary of Key Decisions

| Area | Decision | Key Benefit |
|------|----------|-------------|
| Sampling | Poisson with exponential intervals | Unbiased, size-weighted |
| Interposition | Platform-native (LD_PRELOAD, malloc_logger) | Complete allocation coverage |
| Data Structures | Lock-free hash tables | Zero-contention hot path |
| Free Optimization | Bloom filter | 5× faster non-sampled frees |
| Stack Capture | Frame pointers + mixed-mode | Fast + Python attribution |
| Resolution | Synchronous on stop/snapshot | No lock contention |
| API | Mirror CPU profiler | Consistent user experience |

