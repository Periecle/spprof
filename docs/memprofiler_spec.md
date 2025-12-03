# spprof Memory Allocation Profiler Specification

**Version**: 1.0.7  
**Status**: FINAL (APPROVED - Ready for Implementation)  
**Author**: spprof contributors  
**Date**: December 2024  
**Last Updated**: December 3, 2024

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [Goals and Non-Goals](#2-goals-and-non-goals)
3. [Mathematical Foundation](#3-mathematical-foundation)
4. [Architecture Overview](#4-architecture-overview)
5. [Data Structures](#5-data-structures)
6. [Platform Interposition](#6-platform-interposition)
7. [Sampling Engine](#7-sampling-engine)
8. [Stack Capture and Interning](#8-stack-capture-and-interning)
9. [Python Integration](#9-python-integration)
10. [API Reference](#10-api-reference)
11. [Performance Characteristics](#11-performance-characteristics)
12. [Safety and Correctness](#12-safety-and-correctness)
13. [Implementation Plan](#13-implementation-plan)
14. [Testing Strategy](#14-testing-strategy)
15. [Future Enhancements](#15-future-enhancements)

---

## 1. Executive Summary

The spprof Memory Allocation Profiler is a **production-grade, ultra-low-overhead** memory profiling subsystem that uses **Poisson sampling via native allocator interposition** to provide statistically accurate heap profiling for Python applications and their native extensions.

### Key Features

| Feature | Description |
|---------|-------------|
| **Poisson Sampling** | Mathematically unbiased heap estimation with configurable sampling rate |
| **Native Interposition** | Intercepts malloc/free at the lowest level, capturing all allocations |
| **Lock-Free Hot Path** | Zero locks in malloc/free fast path; ~5-10 cycles overhead |
| **Bloom Filter Optimization** | ~3ns free() path for non-sampled allocations (99.99% of frees) |
| **Complete Coverage** | Python allocations, C extensions, NumPy, Torch, Rust bindings |
| **Production Safe** | Re-entrancy safe, race-free two-phase insert, graceful degradation |
| **Independent Subsystem** | Separate from CPU profiler; can run simultaneously or independently |

### What Makes This Different

Unlike simple memory tracking approaches:

1. **Not `tracemalloc`**: We capture native allocations, not just Python objects
2. **Not per-allocation hooks**: We sample statistically, not every allocation
3. **Not `valgrind/massif`**: We're designed for production (<0.1% overhead)
4. **Not heap snapshots only**: We track allocation lifetime and churn

---

## 2. Goals and Non-Goals

### 2.1 Goals

1. **Ultra-low overhead**: <0.1% CPU overhead at default sampling rate (512KB)
2. **Mathematical rigor**: Unbiased heap estimation with provable error bounds
3. **Complete visibility**: Capture allocations from Python code, C extensions, and system libraries
4. **Production deployment**: Safe to run continuously in production environments
5. **Rich data**: Track allocation sites, sizes, lifetimes, and freed vs. live status
6. **Integration**: Leverage existing spprof infrastructure (resolver, output formats)

### 2.2 Non-Goals

1. **Exact byte counting**: We sample, not count. Trade accuracy for performance.
2. **Object-level tracking**: We track memory regions, not Python object references
3. **Garbage collector integration**: We intercept malloc/free, not Python GC
4. **Memory leak detection**: We provide data; leak detection is analysis layer
5. **Real-time alerting**: We collect data; alerting is separate concern

### 2.3 Important Clarification: Virtual Memory vs RSS

⚠️ **What we measure**: The memory profiler tracks **virtual memory requested** via
`malloc()/calloc()/realloc()`, NOT physical memory (RSS) or page faults.

| Metric | What it means | What we track |
|--------|---------------|---------------|
| **Virtual Memory** | Address space requested from allocator | ✅ Yes |
| **RSS** | Physical pages actually mapped | ❌ No |
| **Huge Pages** | THP or MAP_HUGETLB allocations | ✅ Yes (same as regular) |
| **mmap (direct)** | Memory-mapped files/regions | ❌ No (only malloc) |

**Why this matters:**

1. A 1GB `malloc()` that's never touched uses 0 bytes RSS but shows as 1GB in our profile
2. Applications using Transparent Huge Pages (THP) will show the same virtual size
3. Memory-mapped files (`mmap()` without going through malloc) are not tracked

**For most Python applications**, this distinction is minor because:
- NumPy/PyTorch allocate via malloc (tracked)
- Python objects allocate via PyMem (which uses malloc, tracked)
- Most heap pressure comes from these sources

**For RSS correlation**, compare our `estimated_heap_bytes` with `/proc/self/status` VmRSS.
Large discrepancies indicate either memory-mapped IO or untouched allocations.

### 2.4 Additional Measurement Caveats

**ASLR and Stack ID Stability:**

With Address Space Layout Randomization (ASLR), the same logical call stack will have
**different raw program counters (PCs)** across process restarts. This has implications:

| Scenario | Impact |
|----------|--------|
| Within single process | ✅ Stack IDs are consistent and comparable |
| Across process restarts | ❌ Same logical stack → different stack_id |
| Comparing profiles from different runs | ⚠️ Must compare by symbol name, not stack_id |

**Practical impact**: When comparing heap profiles across runs (e.g., "did we fix the leak?"),
use resolved function names, not raw stack IDs. The resolver produces consistent symbol
names regardless of ASLR randomization.

**Memory-Mapped Regions (mmap Bypass):**

Applications using `mmap()` directly for memory management **bypass malloc entirely**:

```c
/* NOT TRACKED by memory profiler: */
void* p = mmap(NULL, 1GB, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);

/* TRACKED by memory profiler: */
void* q = malloc(1GB);  /* Goes through our hooks */
```

Common sources of untracked memory:
- NumPy memmap arrays (`np.memmap()`)
- Database memory-mapped files (SQLite, LMDB)
- JIT compilers allocating executable pages
- Large allocations that glibc satisfies via mmap internally (threshold ~128KB)

**Note on glibc mmap threshold**: glibc's malloc uses mmap for allocations above
`M_MMAP_THRESHOLD` (default ~128KB). These ARE tracked because they go through
malloc's internal bookkeeping. Only direct `mmap()` calls bypass our hooks.

**Profiler Overhead on Sampled Allocations:**

When we sample an allocation, the ~500ns cold path overhead becomes **part of that
allocation's apparent latency** from the application's perspective. For micro-benchmarks
measuring allocation performance, this can skew results:

```
True allocation time:     50ns
Profiler cold path:      500ns
Apparent allocation time: 550ns (11× slower for sampled allocs!)
```

**Mitigation**: At 512KB sampling rate, only ~0.01% of allocations are sampled.
The aggregate overhead is negligible (<0.1%), but individual sampled allocations
show inflated latency. For allocation micro-benchmarks, disable the profiler.

---

## 3. Mathematical Foundation

### 3.1 Why Poisson Sampling?

The fundamental insight is that **counting every allocation is too expensive** for production use, but **random sampling can give accurate estimates** with bounded error.

**The Problem with Counting Every Allocation:**

```
Typical allocation rate: 1M allocations/second
Cost per allocation hook: ~100 cycles = 30ns
Overhead: 30ms/second = 3% CPU
```

**Poisson Sampling Solution:**

```
Sampling rate: 1 sample per 512KB
Typical allocation rate: 100 MB/s
Samples per second: ~200
Cost per sample: ~1µs
Overhead: 0.0002% CPU
```

### 3.2 The Poisson Process

We model allocation sizes as a stream of bytes. We want to sample this stream such that:

1. Each allocation has probability proportional to its size
2. Large allocations are more likely to be sampled (they matter more)
3. The expected number of samples is predictable

**Algorithm:**

```
Initialize: counter = ExponentialVariate(λ = 1/sampling_rate)

On malloc(size):
    counter -= size
    if counter <= 0:
        SAMPLE this allocation
        counter = ExponentialVariate(λ = 1/sampling_rate)
    return real_malloc(size)
```

### 3.3 Mathematical Properties

Given sampling rate `S` (average bytes between samples):

| Property | Formula |
|----------|---------|
| Expected samples/second | `allocation_rate / S` |
| Sample weight | `S` bytes (what each sample represents) |
| Unbiased heap estimate | `Σ(sampled_size × weight)` |
| Variance | `O(1/√n)` where n = sample count |
| 95% confidence interval | `±1.96 × σ/√n` |

**Proof of Unbiasedness:**

For any allocation of size `s`, the probability it's sampled is `s/S`.
The weight assigned to each sample is `S`.
Expected contribution: `(s/S) × S = s` = actual size. ∎

### 3.4 Exponential Variate Generation

We use the inverse transform method with xorshift128+ PRNG:

```c
// Fast exponential variate: -mean × ln(U) where U ~ Uniform(0,1)
static inline int64_t next_sample_threshold(uint64_t mean_bytes) {
    double u = prng_next_double();  // [0, 1)
    if (u < 1e-15) u = 1e-15;       // Prevent ln(0)
    return (int64_t)(-((double)mean_bytes) * log(u));
}
```

The xorshift128+ PRNG provides:
- Period: 2^128 - 1
- Speed: ~1.5 cycles per 64-bit output
- Quality: Passes BigCrush statistical tests

### 3.5 Error Bounds

For `n` samples with mean sample size `μ` and standard deviation `σ`:

```
Estimated heap = n × S
Standard error = S × √n × (σ/μ)
Relative error = 1/√n × (σ/μ)
```

**Example:**
- 1000 samples with CV (σ/μ) = 2
- Relative error ≈ 6.3%
- 95% CI: estimated ± 12.6%

**Count-Based vs Byte-Weighted Metrics:**

The formulas above apply to **byte-weighted** estimates (total heap size). For
**count-based** metrics (e.g., "how many allocations from function X?"), the
error is significantly higher because:

1. Each sample represents `S/size` allocations (inverse relationship)
2. Small allocations have high count-weight variance
3. The effective sample count for counting is lower than for bytes

| Metric Type | Formula | Typical Error |
|-------------|---------|---------------|
| Bytes (heap size) | `Σ(weight)` | Lower (well-estimated) |
| Count (num allocs) | `Σ(weight/size)` | **Higher** (more variance) |

**Practical guidance**: Trust byte-weighted metrics (heap size, bytes per function).
Treat allocation counts as approximate ("~1000 allocations" not "exactly 1000").

### 3.6 Variance Warning for Short Runs

⚠️ **Important**: Short profiling runs or workloads with many tiny allocations will
have **higher variance** in heap estimates.

**Why this matters:**

| Workload | Allocation Size | Sampling Rate | Samples/10MB | Relative Error |
|----------|-----------------|---------------|--------------|----------------|
| NumPy arrays | 1MB avg | 512KB | ~20 | ~20% |
| String processing | 32B avg | 512KB | ~0.06 | **Very high** |
| Mixed | 10KB avg | 512KB | ~200 | ~7% |

**Small Allocation Bias**: When `allocation_size << sampling_rate`, most allocations
are never sampled. A 10MB heap of 32-byte strings may yield only a handful of samples,
resulting in estimates that could be off by 50% or more in short runs.

**Mitigation strategies:**

1. **Longer profiling runs**: More samples → lower variance (error ∝ 1/√n)
2. **Lower sampling rate**: `sampling_rate_kb=64` increases samples 8× but adds overhead
3. **Document uncertainty**: Always report sample count alongside estimates

**Python API includes variance indicator:**

```python
snapshot = memprof.get_snapshot()
if snapshot.live_samples < 100:
    print(f"⚠️ Low sample count ({snapshot.live_samples}). "
          f"Estimates may have high variance.")
```

---

## 4. Architecture Overview

```
┌─────────────────────────────────────────────────────────────────────────────────┐
│                    spprof Memory Allocation Profiler                            │
├─────────────────────────────────────────────────────────────────────────────────┤
│                                                                                 │
│  ┌───────────────────────────────────────────────────────────────────────────┐ │
│  │                        INTERPOSITION LAYER                                 │ │
│  │  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐      │ │
│  │  │   Linux     │  │   macOS     │  │   Windows   │  │   PyMem     │      │ │
│  │  │ LD_PRELOAD  │  │malloc_logger│  │  Detours    │  │SetAllocator │      │ │
│  │  │  + GOT*     │  │  callback   │  │  IAT Hook   │  │ (optional)  │      │ │
│  │  └──────┬──────┘  └──────┬──────┘  └──────┬──────┘  └──────┬──────┘      │ │
│  │         │                │                │                │             │ │
│  └─────────┴────────────────┴────────────────┴────────────────┴─────────────┘ │
│                                      │                                         │
│                                      ▼                                         │
│  ┌───────────────────────────────────────────────────────────────────────────┐ │
│  │                     SAMPLING ENGINE (Lock-Free)                           │ │
│  │                                                                           │ │
│  │   Thread-Local Storage:                                                   │ │
│  │   ┌─────────────────────────────────────────────────────────────────┐    │ │
│  │   │  int64_t  byte_counter      // Countdown to next sample         │    │ │
│  │   │  uint64_t prng_state[2]     // Per-thread PRNG (xorshift128+)   │    │ │
│  │   │  int      inside_profiler   // Re-entrancy guard                │    │ │
│  │   │  void*    sample_buffer     // Pre-allocated frame buffer       │    │ │
│  │   └─────────────────────────────────────────────────────────────────┘    │ │
│  │                                                                           │ │
│  │   Hot Path (99.99% of calls):           Cold Path (sampled):             │ │
│  │   ┌─────────────────────────┐          ┌─────────────────────────┐      │ │
│  │   │ counter -= size         │          │ capture_stack()         │      │ │
│  │   │ if (counter > 0)        │  ──────▶ │ hash = intern_stack()   │      │ │
│  │   │   return real_malloc()  │          │ heap_map_insert(ptr,    │      │ │
│  │   └─────────────────────────┘          │   hash, size, weight)   │      │ │
│  │                                        │ counter = next_exp()    │      │ │
│  │                                        └─────────────────────────┘      │ │
│  └───────────────────────────────────────────────────────────────────────────┘ │
│                                      │                                         │
│                                      ▼                                         │
│  ┌───────────────────────────────────────────────────────────────────────────┐ │
│  │                    LIVE HEAP MAP (Lock-Free Hash Table)                   │ │
│  │                                                                           │ │
│  │   Open-addressing, linear probing, 2^N slots                              │ │
│  │   ┌─────────┬──────────┬─────────┬──────────┬───────────┬───────────┐    │ │
│  │   │ ptr     │ stack_id │ size    │ weight   │ timestamp │ state     │    │ │
│  │   │ (64-bit)│ (32-bit) │ (32-bit)│ (32-bit) │ (64-bit)  │ (atomic)  │    │ │
│  │   └─────────┴──────────┴─────────┴──────────┴───────────┴───────────┘    │ │
│  │                                                                           │ │
│  │   Operations: insert O(1), lookup O(1), delete O(1)                       │ │
│  │   Concurrency: Lock-free CAS on ptr field, relaxed reads elsewhere       │ │
│  └───────────────────────────────────────────────────────────────────────────┘ │
│                                      │                                         │
│                                      ▼                                         │
│  ┌───────────────────────────────────────────────────────────────────────────┐ │
│  │                    STACK INTERN TABLE (Lock-Free)                         │ │
│  │                                                                           │ │
│  │   Deduplicates stacks: many allocations share same call site             │ │
│  │   Returns uint32_t stack_id for space efficiency                          │ │
│  │                                                                           │ │
│  │   ┌─────────────────────────────────────────────────────────────────┐    │ │
│  │   │ Stack 0: [0x7fff1234, 0x7fff5678, 0x7fff9abc, ...]             │    │ │
│  │   │ Stack 1: [0x7fff2222, 0x7fff3333, ...]                         │    │ │
│  │   │ ...                                                             │    │ │
│  │   └─────────────────────────────────────────────────────────────────┘    │ │
│  └───────────────────────────────────────────────────────────────────────────┘ │
│                                      │                                         │
│                                      ▼                                         │
│  ┌───────────────────────────────────────────────────────────────────────────┐ │
│  │                    ASYNC RESOLVER (Background Thread)                     │ │
│  │                                                                           │ │
│  │   Resolves raw PCs to symbols using existing spprof infrastructure:       │ │
│  │   - Native: dladdr() / DbgHelp (same as CPU profiler)                     │ │
│  │   - Python: framewalker.c captures PyCodeObject* pointers                 │ │
│  │             resolver.c extracts function name, filename, line             │ │
│  │   - Merge: "Trim & Sandwich" algorithm interleaves Python + native        │ │
│  │                                                                           │ │
│  │   Outputs: ResolvedAllocSample with function names, files, lines          │ │
│  └───────────────────────────────────────────────────────────────────────────┘ │
│                                                                                 │
└─────────────────────────────────────────────────────────────────────────────────┘
```

### 4.1 Component Responsibilities

| Component | Responsibility | Thread Safety |
|-----------|---------------|---------------|
| Interposition Layer | Hook malloc/free at OS level | Platform-specific |
| Sampling Engine | Decide which allocations to sample | Lock-free (TLS) |
| Live Heap Map | Track sampled allocations | Lock-free (CAS) |
| Stack Intern Table | Deduplicate call stacks | Lock-free (CAS) |
| Async Resolver | Resolve native symbols | Single-threaded |

### 4.2 Data Flow

```
1. Application calls malloc(size)
       │
       ▼
2. Interposition hook intercepts
       │
       ▼
3. Sampling engine: counter -= size
       │
       ├── counter > 0 ──────────────────────────────┐
       │   (FAST PATH - 99.99% of calls)             │
       │                                              │
       ▼                                              ▼
4. counter <= 0 (SAMPLE)                         real_malloc(size)
       │                                              │
       ▼                                              │
5. ptr = real_malloc(size)                           │
       │                                              │
       ▼                                              │
6. heap_map_reserve(ptr)  ◄─── CRITICAL: Reserve     │
       │                       slot BEFORE any        │
       ▼                       other thread can       │
7. capture_native_stack()      free(ptr)             │
       │                                              │
       ▼                                              │
8. stack_intern(frames) → stack_id                   │
       │                                              │
       ▼                                              │
9. heap_map_finalize(ptr, stack_id, size, weight)    │
       │                                              │
       ▼                                              │
10. counter = next_exponential_variate()             │
       │                                              │
       └──────────────────────────────────────────────┘
       │
       ▼
11. return ptr to application
```

**Two-Phase Insert & The ABA Problem:**

The primary concurrency risk is the **ABA problem** with virtual address reuse:

```
Thread A (free):              Thread B (malloc):
1. free(p) called
2. real_free(p) executes
   [address p now available]
                              3. p = real_malloc() [same address!]
                              4. heap_map_reserve(p)
5. handle_free(p) runs
   [WRONG: removes Thread B's entry!]
```

**Solution: Wait-Free "Death During Birth" Pattern**

Rather than spinning (dangerous in allocator hooks), we use a wait-free state machine:

```c
/* State transitions:
 *   EMPTY -> RESERVED      (malloc: CAS success)
 *   TOMBSTONE -> RESERVED  (malloc: CAS success, REUSE - see below)
 *   RESERVED -> ptr        (malloc: finalize, CAS success)  
 *   RESERVED -> TOMBSTONE  (free: "death during birth")
 *   ptr -> TOMBSTONE       (free: normal path)
 *
 * TOMBSTONE REUSE POLICY:
 *
 * Original design had "TOMBSTONE -> EMPTY (compaction only)" but this
 * creates a performance cliff: as tombstones accumulate, probe sequences
 * lengthen, eventually causing O(N) lookups and "table full" drops even
 * at low load factors.
 *
 * SOLUTION: Allow heap_map_reserve to recycle TOMBSTONE slots directly.
 * This is safe because:
 *   1. The CAS guarantees atomicity (only one thread wins the slot)
 *   2. The birth_seq in the new entry is always > any previous free_seq
 *   3. The "Zombie Killer" logic uses sequence numbers, not slot identity
 *
 * This eliminates the need for explicit compaction (which is hard to do
 * correctly in a lock-free hash map) and keeps performance O(1).
 */

/* Phase 1: Reserve slot immediately after real_malloc returns */
int heap_map_reserve(uintptr_t ptr) {
    uint64_t idx = hash_ptr(ptr) & MEMPROF_HEAP_MAP_MASK;
    
    for (int probe = 0; probe < MEMPROF_MAX_PROBE; probe++) {
        HeapMapEntry* entry = &g_memprof.heap_map[idx];
        uintptr_t current = atomic_load_explicit(&entry->ptr, memory_order_relaxed);
        
        /* Try to claim EMPTY or TOMBSTONE slots */
        if (current == HEAP_ENTRY_EMPTY || current == HEAP_ENTRY_TOMBSTONE) {
        if (atomic_compare_exchange_strong_explicit(
                    &entry->ptr, &current, HEAP_ENTRY_RESERVED,
                memory_order_acq_rel, memory_order_relaxed)) {
            /* Slot claimed. Store ptr temporarily in metadata for matching. */
            atomic_store_explicit(&entry->metadata, (uint64_t)ptr, 
                                  memory_order_release);
                /* Track tombstone recycling for diagnostics */
                if (current == HEAP_ENTRY_TOMBSTONE) {
                    atomic_fetch_add(&g_memprof.tombstones_recycled, 1);
                }
            return (int)idx;  /* Return slot index for finalize */
            }
            /* CAS failed - another thread claimed it, continue probing */
        }
        idx = (idx + 1) & MEMPROF_HEAP_MAP_MASK;
    }
    return -1;  /* Table full (all probed slots are OCCUPIED or RESERVED) */
}

/* Phase 2: Finalize with metadata (after stack capture) */
int heap_map_finalize(int slot_idx, uintptr_t ptr, uint64_t packed_metadata) {
    HeapMapEntry* entry = &g_memprof.heap_map[slot_idx];
    
    /* Store metadata first (relaxed OK, ptr publish provides release) */
    atomic_store_explicit(&entry->metadata, packed_metadata, memory_order_relaxed);
    atomic_store_explicit(&entry->timestamp, get_monotonic_ns(), memory_order_relaxed);
    
    /* Publish: CAS RESERVED -> ptr. If this fails, "death during birth" occurred. */
    uintptr_t expected = HEAP_ENTRY_RESERVED;
    if (!atomic_compare_exchange_strong_explicit(
            &entry->ptr, &expected, ptr,
            memory_order_release, memory_order_relaxed)) {
        /* Slot was tombstoned by free() - allocation died during birth.
         * Clean up: entry is already TOMBSTONE, just update stats. */
        atomic_fetch_sub(&g_memprof.heap_map_insertions, 1);
        return 0;  /* Indicate birth failure */
    }
    return 1;  /* Success */
}

/* Free handling: Wait-free, never spins
 * 
 * free_seq: The global sequence number captured at the START of this free hook.
 *           Used for deterministic zombie detection on macOS post-hooks.
 */
int heap_map_remove(uintptr_t ptr, uint64_t free_seq, uint64_t free_timestamp,
                    uint32_t* out_stack_id, uint32_t* out_size, 
                    uint32_t* out_weight, uint64_t* out_duration) {
    uint64_t idx = hash_ptr(ptr) & MEMPROF_HEAP_MAP_MASK;
    
    for (int probe = 0; probe < MEMPROF_MAX_PROBE; probe++) {
        HeapMapEntry* entry = &g_memprof.heap_map[idx];
        uintptr_t entry_ptr = atomic_load_explicit(&entry->ptr, memory_order_acquire);
        
        if (entry_ptr == ptr) {
            /* Found it - but is this the SAME allocation we freed, or a new one
             * that reused the address? (macOS "Zombie Killer" race)
             *
             * On macOS malloc_logger, we're a POST-HOOK: real_free() already 
             * returned, so the address could have been reused by another thread's
             * malloc() before our handle_free() runs.
             *
             * DETERMINISTIC SOLUTION: Use global sequence counter.
             * If entry->birth_seq > free_seq, this allocation was BORN after
             * our free was issued, so it's a different allocation entirely.
             */
            uint64_t entry_birth_seq = atomic_load_explicit(&entry->birth_seq, 
                                                            memory_order_relaxed);
            if (entry_birth_seq > free_seq) {
                /* Entry was created AFTER our free was issued - zombie race!
                 * This is a new allocation, not the one we freed. */
                atomic_fetch_add(&g_memprof.zombie_races_detected, 1);
                return 0;  /* Ignore this zombie free */
            }
            
            /* Safe to remove - normal removal path */
            atomic_store_explicit(&entry->ptr, HEAP_ENTRY_TOMBSTONE, 
                                  memory_order_release);
            /* Extract metadata for caller... */
            uint64_t entry_ts = atomic_load_explicit(&entry->timestamp,
                                                      memory_order_relaxed);
            *out_duration = free_timestamp - entry_ts;
            return 1;
        }
        
        if (entry_ptr == HEAP_ENTRY_RESERVED) {
            /* Check if this RESERVED slot is for our ptr (stored in metadata) */
            uint64_t reserved_ptr = atomic_load_explicit(&entry->metadata, 
                                                          memory_order_acquire);
            if (reserved_ptr == (uint64_t)ptr) {
                /* "Death during birth" - tombstone the RESERVED slot.
                 * The allocating thread's finalize() will see this and clean up. */
                atomic_store_explicit(&entry->ptr, HEAP_ENTRY_TOMBSTONE,
                                      memory_order_release);
                return 1;  /* Successfully "freed" the in-flight allocation */
            }
        }
        
        if (entry_ptr == HEAP_ENTRY_EMPTY) {
            return 0;  /* Not found (wasn't sampled) */
        }
        
        idx = (idx + 1) & MEMPROF_HEAP_MAP_MASK;
    }
    return 0;  /* Not found after max probes */
}
```

**Pre-Hook vs Post-Hook Requirement:**

| Platform | Hook Timing | ABA Risk |
|----------|-------------|----------|
| Linux LD_PRELOAD | Pre-hook (we call `real_free` after our logic) | ✅ Minimal |
| macOS malloc_logger | Post-hook (callback after malloc/free complete) | ⚠️ High |
| Windows Detours | Pre-hook (we control call order) | ✅ Minimal |

**macOS "Zombie Killer" Race (Post-Hook ABA):**

The "Death During Birth" pattern handles the case where `free()` races with an in-flight
`malloc()` that's still in RESERVED state. However, macOS has a more severe race because
`malloc_logger` is a **post-hook** (runs after `real_free` returns):

```
Thread A (free Old_P):           Thread B (malloc New_P):
1. free(Old_P) called
2. real_free(Old_P) completes
   [address P now in free list]
                                 3. malloc() -> gets P (reuse!)
                                 4. heap_map_reserve(P) -> RESERVED
                                 5. capture_stack()
                                 6. heap_map_finalize(P) -> OCCUPIED
7. handle_free(P) callback runs
8. Finds P in OCCUPIED state
9. WRONG: Tombstones Thread B's NEW allocation!
```

**Solution: Global Sequence Counter**

Since `malloc_logger` doesn't provide a unique allocation ID, we use a global sequence
counter for **deterministic** zombie detection:

```c
/* Global monotonic sequence counter - incremented on every malloc/free hook entry */
static _Atomic uint64_t g_global_seq = 0;

/* In malloc hook (after real_malloc returns): */
uint64_t birth_seq = atomic_fetch_add(&g_global_seq, 1, memory_order_relaxed);
/* Store birth_seq in heap map entry alongside ptr */

/* In free hook (entry point, before any work): */
uint64_t free_seq = atomic_fetch_add(&g_global_seq, 1, memory_order_relaxed);
/* Later, when we find an entry: */
if (entry->birth_seq > free_seq) {
    /* This entry was BORN after our free was issued - zombie race!
     * The allocation we freed was OLD, this is a NEW allocation. */
    atomic_fetch_add(&g_memprof.zombie_races_detected, 1);
    return 0;  /* Ignore this zombie free */
}
```

**Why sequence counter over timestamps?**

| Approach | Cost | Correctness |
|----------|------|-------------|
| Timestamp heuristic | ~1ns (clock read) | Probabilistic (~99.9%) |
| Sequence counter | ~3-5ns (atomic add) | **Deterministic (100%)** |

The timestamp heuristic fails in edge cases:
- Thread preempted for exactly EPSILON nanoseconds → false positive/negative
- Clock skew between cores → unpredictable behavior
- Very fast alloc/free loops → legitimate frees incorrectly ignored

In Python's context (GIL overhead, already heavy per-call cost), an extra 3ns atomic
instruction is negligible. **Correctness over performance** is the right trade-off here.

**HeapMapEntry update for sequence ID:**

```c
/* Packed metadata now includes birth_seq (replaces timestamp for ordering) */
typedef struct {
    _Atomic uintptr_t ptr;        /* Key: allocated pointer */
    _Atomic uint64_t  metadata;   /* Packed: stack_id | size | weight */
    _Atomic uint64_t  birth_seq;  /* Sequence number at allocation time */
    uint64_t          timestamp;  /* Wall clock time (for duration reporting only) */
} HeapMapEntry;
```

Note: `timestamp` is retained for reporting allocation lifetimes in human-readable form,
but `birth_seq` is used for correctness-critical zombie detection.

---

## 5. Data Structures

### 5.1 Configuration Constants

```c
/* Maximum native stack depth to capture */
#define MEMPROF_MAX_STACK_DEPTH 64

/* Live heap map capacity (must be power of 2) */
#define MEMPROF_HEAP_MAP_CAPACITY (1 << 20)  /* 1M entries, ~32MB */
#define MEMPROF_HEAP_MAP_MASK (MEMPROF_HEAP_MAP_CAPACITY - 1)

/* Stack intern table capacity - DYNAMIC SIZING
 *
 * Memory footprint was a major concern: 256K entries × 544B = 140MB is excessive
 * for small scripts. We now use dynamic sizing:
 *
 *   - Initial: 4K entries (~2MB) - suitable for most scripts
 *   - Growth: Double when >75% full, up to max
 *   - Maximum: Configurable via SPPROF_STACK_TABLE_MAX env var
 *   - Default max: 64K entries (~35MB) - covers most production apps
 *   - Large apps: Set SPPROF_STACK_TABLE_MAX=262144 for 256K entries
 *
 * PLATFORM-SPECIFIC RESIZE IMPLEMENTATION:
 *
 * | Platform | Method | Notes |
 * |----------|--------|-------|
 * | Linux | mremap(MREMAP_MAYMOVE) | Efficient, may move address |
 * | macOS | mmap new + memcpy + munmap old | No mremap, must copy |
 * | Windows | VirtualAlloc + memcpy + VirtualFree | Same as macOS |
 *
 * On non-Linux platforms, resize has O(n) copy cost. To minimize impact:
 *   1. Resize is done on background thread (not in sampling hot path)
 *   2. Use read-copy-update: new table is built, then atomically swapped
 *   3. Old table is "leaked" until quiescent (same pattern as Bloom filter)
 *
 * The resize operation should block <10ms for typical tables (35MB @ 1GB/s memcpy).
 */
#define MEMPROF_STACK_TABLE_INITIAL  (1 << 12)  /* 4K entries (~2MB) */
#define MEMPROF_STACK_TABLE_MAX_DEFAULT (1 << 16)  /* 64K entries (~35MB) */
#define MEMPROF_STACK_TABLE_GROW_THRESHOLD 75  /* Grow at 75% load */

/* Alternative: Compact stack storage (Structure of Arrays)
 *
 * Most stacks are <20 frames, but we store 64 frames per entry (512B waste).
 * Future optimization: Store frames in a separate dense buffer:
 *
 *   StackEntryCompact: { hash, depth, frame_offset, flags } = 16B
 *   FrameBuffer: contiguous uintptr_t array, indexed by frame_offset
 *
 * This reduces per-entry overhead from ~544B to ~16B + (depth × 8B).
 * Trade-off: More complex allocation, harder to iterate.
 */

/* Probe limit for open-addressing */
#define MEMPROF_MAX_PROBE 128

/* Default sampling rate (bytes between samples) */
#define MEMPROF_DEFAULT_SAMPLING_RATE (512 * 1024)  /* 512 KB */
```

### 5.2 Heap Map Entry

```c
/**
 * HeapMapEntry - Single entry in the live heap map
 *
 * State machine for `ptr` field:
 *   0         = EMPTY (slot available)
 *   valid ptr = OCCUPIED (allocation tracked)
 *   ~0ULL     = TOMBSTONE (freed, slot reusable after compaction)
 *
 * CONCURRENCY MODEL:
 *
 * To avoid torn reads during concurrent free/snapshot operations, we pack
 * stack_id, size, and weight into a single 64-bit atomic field. This ensures
 * snapshot readers always see consistent metadata even without locks.
 *
 * Memory ordering:
 *   - insert: CAS on ptr (acquire-release), then release store on metadata
 *   - lookup: acquire load of ptr, acquire load of metadata
 *   - delete: release store of TOMBSTONE to ptr
 *   - snapshot: acquire loads only - may see stale but consistent data
 *
 * The two-phase insert (reserve then finalize) prevents the free-before-insert
 * race where free() could miss an allocation that was sampled but not yet
 * inserted into the heap map.
 */

/* Packed metadata: stack_id (20 bits) | size (24 bits) | weight (20 bits) */
#define METADATA_PACK(stack_id, size, weight) \
    ((((uint64_t)(stack_id) & 0xFFFFF) << 44) | \
     (((uint64_t)(size) & 0xFFFFFF) << 20) | \
     ((uint64_t)(weight) & 0xFFFFF))

#define METADATA_STACK_ID(m) (((m) >> 44) & 0xFFFFF)
#define METADATA_SIZE(m)     (((m) >> 20) & 0xFFFFFF)
#define METADATA_WEIGHT(m)   ((m) & 0xFFFFF)

/* Maximum values due to bit packing */
#define MAX_STACK_ID   ((1 << 20) - 1)  /* ~1M unique stacks */
#define MAX_ALLOC_SIZE ((1 << 24) - 1)  /* 16MB (larger sizes clamped) */
#define MAX_WEIGHT     ((1 << 20) - 1)  /* ~1M (sufficient for 1TB sampling) */

typedef struct {
    _Atomic uintptr_t ptr;        /* Key: allocated pointer (EMPTY/OCCUPIED/TOMBSTONE) */
    _Atomic uint64_t  metadata;   /* Packed: stack_id | size | weight (atomic for consistency) */
    _Atomic uint64_t  timestamp;  /* Allocation time (monotonic ns) */
} HeapMapEntry;

#define HEAP_ENTRY_EMPTY     ((uintptr_t)0)
#define HEAP_ENTRY_TOMBSTONE (~(uintptr_t)0)
#define HEAP_ENTRY_RESERVED  ((uintptr_t)1)  /* Placeholder during insert */
```

**Memory Layout (24 bytes per entry, cache-line friendly):**

```
┌────────────────┬────────────────────────────┬────────────────┐
│   ptr (8B)     │   metadata (8B packed)     │  timestamp (8B)│
│                │ [stack_id|size|weight]     │                │
└────────────────┴────────────────────────────┴────────────────┘
```

**Total heap map memory**: 1M entries × 24B = **24 MB** (reduced from 40MB)

**Bit Packing Trade-offs:**

| Field | Bits | Max Value | Rationale |
|-------|------|-----------|-----------|
| stack_id | 20 | 1,048,575 | More than stack table capacity |
| size | 24 | 16,777,215 | 16MB max; larger allocations clamped (see note) |
| weight | 20 | 1,048,575 | Supports up to 1TB sampling intervals |

**⚠️ Size Clamping Note:**

Allocations larger than 16MB are **clamped** to 16MB in the `size` field for display purposes.
This is a limitation of the 24-bit packed representation.

**Impact on heap estimation**: **None**. The `weight` field (sampling rate) is used for heap
estimation (`Σ(weight)`), not the size field. A 100MB allocation sampled at 512KB rate
contributes 512KB to the estimated heap, regardless of what's stored in `size`.

**Impact on per-allocation display**: The displayed size will be incorrect:
```
Displayed:  "numpy.zeros: 16MB (sample weight 512KB)"
Reality:    "numpy.zeros: 100MB (sample weight 512KB)"
```

**Mitigation**: For large allocations (>16MB), check the `actual_size` field in
`AllocationSample` which stores the unclamped value (requires separate storage).
In v1.0, this limitation is documented but not mitigated. Future versions may use
log-scale encoding: `stored = log2(size) * scale_factor` to support 0-4GB range.

### 5.3 Bloom Filter (Free Path Optimization)

**Problem**: Every `free(ptr)` requires a hash map lookup to check if the pointer was sampled. Most frees (~99.99%) are for non-sampled allocations, but we still pay the cache miss cost (~15ns) of probing the hash map. At 1M frees/second, this adds 15ms overhead.

**Solution**: A Bloom filter provides O(1) negative answers with zero false negatives:

```c
/**
 * Bloom Filter for free() hot path optimization.
 *
 * Parameters chosen for:
 *   - 1M bits = 128KB memory (fits in L2 cache)
 *   - 4 hash functions (optimal for our load factor)
 *   - False positive rate: ~2% at 50K live entries
 *   - False negative rate: 0% (guaranteed)
 *
 * Trade-off: 2% of non-sampled frees will still probe heap map.
 * This is acceptable since heap map probe is fast when found empty.
 *
 * IMPLEMENTATION NOTE: The filter is accessed via pointer indirection
 * (g_memprof.bloom_filter_ptr) to support atomic swap during rebuilds.
 * Both initial and rebuilt filters are mmap'd for consistency and to
 * avoid mixing allocation strategies.
 */
#define BLOOM_SIZE_BITS (1 << 20)       /* 1M bits */
#define BLOOM_SIZE_BYTES (BLOOM_SIZE_BITS / 8)  /* 128KB */
#define BLOOM_HASH_COUNT 4

/* Filter accessed via g_memprof.bloom_filter_ptr - see Global State (5.6) */

/**
 * Double-hashing scheme: h(i) = h1 + i*h2
 * Uses multiplicative hashing with two different constants.
 */
static inline void bloom_get_indices(uintptr_t ptr, uint64_t indices[BLOOM_HASH_COUNT]) {
    uint64_t h1 = ptr * 0x9E3779B97F4A7C15ULL;  /* Golden ratio */
    uint64_t h2 = ptr * 0xC96C5795D7870F42ULL;  /* Another prime */
    
    for (int i = 0; i < BLOOM_HASH_COUNT; i++) {
        indices[i] = (h1 + i * h2) & (BLOOM_SIZE_BITS - 1);
    }
}

/**
 * Add pointer to Bloom filter (called during sample insert).
 * Uses atomic OR for thread safety.
 *
 * NOTE: Uses pointer indirection through g_memprof.bloom_filter_ptr.
 * This adds one pointer load (~1 cycle) but enables atomic filter swap.
 */
static inline void bloom_add(uintptr_t ptr) {
    _Atomic uint8_t* filter = atomic_load_explicit(&g_memprof.bloom_filter_ptr,
                                                    memory_order_acquire);
    uint64_t indices[BLOOM_HASH_COUNT];
    bloom_get_indices(ptr, indices);
    
    for (int i = 0; i < BLOOM_HASH_COUNT; i++) {
        uint64_t byte_idx = indices[i] / 8;
        uint8_t bit_mask = 1 << (indices[i] % 8);
        atomic_fetch_or_explicit(&filter[byte_idx], bit_mask, 
                                 memory_order_relaxed);
    }
}

/**
 * Check if pointer MIGHT be in set.
 * Returns: 0 = definitely NOT sampled (fast path)
 *          1 = maybe sampled (check heap map)
 *
 * NOTE: Uses pointer indirection. The acquire load of bloom_filter_ptr
 * synchronizes with the release store in bloom_rebuild_from_heap().
 */
static inline int bloom_might_contain(uintptr_t ptr) {
    _Atomic uint8_t* filter = atomic_load_explicit(&g_memprof.bloom_filter_ptr,
                                                    memory_order_acquire);
    uint64_t indices[BLOOM_HASH_COUNT];
    bloom_get_indices(ptr, indices);
    
    for (int i = 0; i < BLOOM_HASH_COUNT; i++) {
        uint64_t byte_idx = indices[i] / 8;
        uint8_t bit_mask = 1 << (indices[i] % 8);
        uint8_t byte_val = atomic_load_explicit(&filter[byte_idx],
                                                 memory_order_relaxed);
        if (!(byte_val & bit_mask)) {
            return 0;  /* Definitely NOT in set */
        }
    }
    return 1;  /* Maybe in set - check heap map */
}

/**
 * Optimized free path using Bloom filter.
 */
void handle_free(void* ptr) {
    if (!bloom_might_contain((uintptr_t)ptr)) {
        return;  /* FAST PATH: not sampled, skip heap map lookup */
    }
    /* Bloom says maybe - check heap map (2% false positive rate) */
    heap_map_remove((uintptr_t)ptr, ...);
}
```

**Performance Impact:**

| Metric | Without Bloom | With Bloom |
|--------|---------------|------------|
| Free hot path | ~15ns (hash + probe) | ~3ns (Bloom check) |
| Cache misses/free | ~1 (heap map probe) | ~0.02 (false positives only) |
| Memory overhead | 0 | 128KB |

**Bloom Filter Saturation & Rebuild Strategy:**

In long-running processes (weeks/months), address reuse causes the Bloom filter to saturate,
degrading the fast path until it approaches 100% false positives.

```c
/* Saturation monitoring - extends bloom_add with bit counting */
static inline void bloom_add_with_saturation(uintptr_t ptr) {
    _Atomic uint8_t* filter = atomic_load_explicit(&g_memprof.bloom_filter_ptr,
                                                    memory_order_acquire);
    uint64_t indices[BLOOM_HASH_COUNT];
    bloom_get_indices(ptr, indices);
    
    for (int i = 0; i < BLOOM_HASH_COUNT; i++) {
        uint64_t byte_idx = indices[i] / 8;
        uint8_t bit_mask = 1 << (indices[i] % 8);
        uint8_t old_val = atomic_fetch_or_explicit(&filter[byte_idx], 
                                                    bit_mask, memory_order_relaxed);
        /* Track new bits (approximate - may double-count under contention) */
        if (!(old_val & bit_mask)) {
            atomic_fetch_add_explicit(&g_memprof.bloom_ones_count, 1, 
                                      memory_order_relaxed);
        }
    }
}

/* Check if rebuild is needed (>50% saturation) */
static inline int bloom_needs_rebuild(void) {
    uint64_t ones = atomic_load_explicit(&g_bloom_ones_count, memory_order_relaxed);
    return ones > (BLOOM_SIZE_BITS / 2);
}

/**
 * Rebuild Bloom filter from live heap map (background task).
 *
 * Called when saturation exceeds threshold. Steps:
 * 1. Allocate clean filter
 * 2. Iterate heap map, add all live pointers
 * 3. Atomic swap filter pointer
 * 4. INTENTIONALLY LEAK old filter (see safety note)
 *
 * THREAD SAFETY: Can run concurrently with bloom_might_contain() (readers
 * see either old or new filter, both correct). bloom_add() must be paused
 * briefly during swap (use RCU-like epoch or just accept brief inconsistency).
 *
 * CRITICAL SAFETY NOTE - Why we don't munmap():
 * 
 * The naive approach of "sleep then munmap" is UNSAFE:
 *   1. Thread A loads old_filter pointer
 *   2. Thread A gets preempted (priority inversion, high load, etc.)
 *   3. Rebuilder swaps ptr, sleeps 1ms, munmap(old_filter)
 *   4. Thread A wakes, dereferences old_filter -> SIGSEGV
 *
 * A fixed sleep cannot guarantee all readers have finished. True RCU
 * (quiescent state detection) is complex and adds overhead.
 *
 * SOLUTION: Intentionally leak old filters. Each rebuild leaks 128KB.
 * At ~1 rebuild per week, a process running for months leaks ~1-2MB.
 * This is an acceptable trade-off for 0% crash risk.
 *
 * Old filters ARE cleaned up at process exit (memprof_shutdown walks the
 * leaked filter list) or can be manually reclaimed during quiescent periods.
 */

/* Linked list of leaked filters for optional cleanup */
typedef struct LeakedFilter {
    _Atomic uint8_t* filter;
    struct LeakedFilter* next;
} LeakedFilter;

static _Atomic(LeakedFilter*) g_leaked_filters = NULL;

static void record_leaked_filter(_Atomic uint8_t* filter) {
    LeakedFilter* node = mmap(NULL, sizeof(LeakedFilter),
                               PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (node == MAP_FAILED) return;  /* Best effort */
    
    node->filter = filter;
    LeakedFilter* old_head;
    do {
        old_head = atomic_load(&g_leaked_filters);
        node->next = old_head;
    } while (!atomic_compare_exchange_weak(&g_leaked_filters, &old_head, node));
}

int bloom_rebuild_from_heap(void) {
    /* Allocate new filter */
    _Atomic uint8_t* new_filter = mmap(NULL, BLOOM_SIZE_BYTES, 
                                        PROT_READ | PROT_WRITE,
                                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (new_filter == MAP_FAILED) return -1;
    
    /* Iterate heap map, add live entries to new filter */
    uint64_t new_ones = 0;
    for (size_t i = 0; i < MEMPROF_HEAP_MAP_CAPACITY; i++) {
        uintptr_t ptr = atomic_load_explicit(&g_memprof.heap_map[i].ptr,
                                              memory_order_relaxed);
        if (ptr != HEAP_ENTRY_EMPTY && ptr != HEAP_ENTRY_TOMBSTONE &&
            ptr != HEAP_ENTRY_RESERVED) {
            /* Add to new filter.
             * 
             * NON-ATOMIC ACCESS NOTE: Although new_filter is declared as
             * `_Atomic uint8_t*`, we access it non-atomically here. This is
             * safe because:
             *   1. new_filter is not published yet (still private to this thread)
             *   2. No other thread can see or access it until atomic_store below
             *   3. The atomic_store with release semantics will synchronize all
             *      prior writes to new_filter before any reader can see the pointer
             *
             * For clarity, we could cast to non-atomic for these writes:
             *   uint8_t* filter_raw = (uint8_t*)new_filter;
             * But the behavior is identical since no synchronization is needed yet.
             */
            uint64_t indices[BLOOM_HASH_COUNT];
            bloom_get_indices(ptr, indices);
            for (int j = 0; j < BLOOM_HASH_COUNT; j++) {
                uint64_t byte_idx = indices[j] / 8;
                uint8_t bit_mask = 1 << (indices[j] % 8);
                /* Non-atomic read/modify/write - safe before publication */
                uint8_t* byte_ptr = (uint8_t*)&new_filter[byte_idx];
                if (!(*byte_ptr & bit_mask)) {
                    *byte_ptr |= bit_mask;
                    new_ones++;
                }
            }
        }
    }
    
    /* Atomic swap - readers see either old or new, both valid */
    _Atomic uint8_t* old_filter = atomic_load_explicit(&g_memprof.bloom_filter_ptr,
                                                        memory_order_relaxed);
    atomic_store_explicit(&g_memprof.bloom_filter_ptr, new_filter, memory_order_release);
    atomic_store_explicit(&g_memprof.bloom_ones_count, new_ones, memory_order_relaxed);
    
    /* INTENTIONALLY LEAK old_filter - record for optional cleanup at shutdown */
    record_leaked_filter(old_filter);
    atomic_fetch_add(&g_memprof.bloom_rebuilds, 1);
    
    return 0;
}

/* Called during memprof_shutdown() - safe because all threads have stopped.
 *
 * LSAN/ASAN COMPLIANCE:
 * 
 * The intentional leak strategy is pragmatically correct for runtime safety,
 * but LeakSanitizer (LSan) will report these as leaks. To ensure clean test
 * runs and satisfy "zero leak" policies:
 *
 * 1. This function is called by memprof_shutdown()
 * 2. All leaked filters are tracked in g_leaked_filters list
 * 3. shutdown() cleans them up when it's safe (all threads stopped)
 *
 * This gives us BOTH runtime safety (no use-after-free during operation)
 * AND clean sanitizer reports (no leaks at exit).
 */
void bloom_cleanup_leaked_filters(void) {
    LeakedFilter* node = atomic_load(&g_leaked_filters);
    while (node) {
        LeakedFilter* next = node->next;
        munmap((void*)node->filter, BLOOM_SIZE_BYTES);
        munmap(node, sizeof(LeakedFilter));
        node = next;
    }
    atomic_store(&g_leaked_filters, NULL);
    
    /* Also free the current active filter */
    _Atomic uint8_t* current = atomic_load(&g_memprof.bloom_filter_ptr);
    if (current) {
        munmap((void*)current, BLOOM_SIZE_BYTES);
        atomic_store(&g_memprof.bloom_filter_ptr, NULL);
    }
}
```

**Rebuild Trigger Policy:**

| Saturation | Action |
|------------|--------|
| <25% | Normal operation |
| 25-50% | Log warning, continue |
| >50% | Trigger background rebuild |
| >75% | Force synchronous rebuild on next sample |

For typical production workloads, rebuilds occur every few days to weeks.
The rebuild takes ~10ms for 1M heap map entries (single-threaded scan).

### 5.4 Stack Intern Table Entry

```c
/**
 * StackEntry - Interned call stack
 *
 * Many allocations share the same call site. Interning saves memory
 * and enables O(1) stack comparison via stack_id.
 *
 * Capacity planning:
 * - 256K entries handles most production workloads
 * - For JIT-heavy apps (PyTorch, JAX), consider compile-time increase to 1M
 * - No eviction policy: stacks persist until profiler shutdown
 */
typedef struct {
    _Atomic uint64_t hash;        /* FNV-1a hash for lookup */
    uint16_t depth;               /* Number of valid frames */
    uint16_t flags;               /* RESOLVED, PYTHON_ATTRIBUTED, etc. */
    uintptr_t frames[MEMPROF_MAX_STACK_DEPTH];  /* Raw return addresses */
    
    /* Resolved symbols (lazily populated by async resolver) */
    char** function_names;        /* Array of function name strings */
    char** file_names;            /* Array of file name strings */
    int*   line_numbers;          /* Array of line numbers */
} StackEntry;

#define STACK_FLAG_RESOLVED        0x0001
#define STACK_FLAG_PYTHON_ATTR     0x0002
#define STACK_FLAG_TRUNCATED       0x0004
```

### 5.5 Thread-Local Sampler State

```c
/**
 * MemProfThreadState - Per-thread sampling state
 *
 * This is the ONLY mutable state accessed in the hot path.
 * All fields are thread-local, no synchronization needed.
 */
typedef struct {
    /* Sampling state */
    int64_t  byte_counter;        /* Countdown to next sample (signed!) */
    uint64_t prng_state[2];       /* xorshift128+ PRNG state */
    
    /* Safety */
    int      inside_profiler;     /* Re-entrancy guard */
    int      initialized;         /* TLS initialized flag */
    
    /* Pre-allocated sample buffer (avoids malloc in cold path) */
    uintptr_t frame_buffer[MEMPROF_MAX_STACK_DEPTH];
    int       frame_depth;
    
    /* Per-thread statistics */
    uint64_t total_allocs;        /* Total allocations seen */
    uint64_t total_frees;         /* Total frees seen */
    uint64_t sampled_allocs;      /* Allocations sampled */
    uint64_t sampled_bytes;       /* Bytes represented by samples */
    uint64_t skipped_reentrant;   /* Calls skipped due to re-entrancy */
} MemProfThreadState;
```

### 5.6 Global Profiler State

```c
/**
 * MemProfGlobalState - Singleton profiler state
 *
 * Immutable after init except for atomic state flags.
 */
typedef struct {
    /* Configuration (immutable after init) */
    uint64_t sampling_rate;       /* Average bytes between samples */
    int      capture_python;      /* Also hook PyMem allocator */
    int      resolve_on_stop;     /* Resolve symbols when profiling stops */
    
    /* State (atomic) - Separate flags for alloc/free tracking
     * This enables stop() to disable new allocations while continuing
     * to track frees, preventing "fake leaks" in the report. */
    _Atomic int active_alloc;     /* Track new allocations (start→stop) */
    _Atomic int active_free;      /* Track frees (start→shutdown) */
    _Atomic int initialized;      /* Init completed */
    
    /* Data structures (allocated once) */
    HeapMapEntry* heap_map;       /* Live allocations (mmap'd) */
    StackEntry*   stack_table;    /* Interned stacks (mmap'd) */
    _Atomic uint32_t stack_count; /* Number of unique stacks */
    
    /* Bloom filter (swappable for rebuild)
     * NOTE: Both initial and rebuilt filters are mmap'd for consistency.
     * This simplifies code paths and enables uniform munmap() cleanup. */
    _Atomic(_Atomic uint8_t*) bloom_filter_ptr;  /* Current active filter (mmap'd) */
    _Atomic uint64_t bloom_ones_count;           /* Approximate bits set (saturation) */
    _Atomic int bloom_rebuild_in_progress;       /* Rebuild lock */
    
    /* Global statistics (atomic) */
    _Atomic uint64_t total_samples;
    _Atomic uint64_t total_frees_tracked;
    _Atomic uint64_t heap_map_collisions;
    _Atomic uint64_t heap_map_insertions;
    _Atomic uint64_t heap_map_deletions;
    _Atomic uint64_t heap_map_full_drops;
    _Atomic uint64_t stack_table_collisions;
    _Atomic uint64_t bloom_rebuilds;         /* Number of filter rebuilds */
    _Atomic uint64_t death_during_birth;     /* Allocs freed before finalize */
    _Atomic uint64_t zombie_races_detected;  /* macOS ABA races caught by sequence guard */
    _Atomic uint64_t tombstones_recycled;    /* TOMBSTONE slots reused (avoids compaction) */
    _Atomic uint64_t shallow_stack_warnings; /* Native stacks truncated (missing frame pointers) */
    
    /* Compaction monitoring (optional, for long-running processes) */
    _Atomic uint64_t compaction_moves;       /* Entries moved by compaction */
    _Atomic uint64_t max_probe_length;       /* Longest probe sequence observed */
    _Atomic uint64_t probe_length_warnings;  /* Probes exceeding warning threshold */
    
    /* Platform-specific state */
    void* platform_state;
} MemProfGlobalState;

/* Global instance */
extern MemProfGlobalState g_memprof;

/* Initialization mmap's the initial Bloom filter */
int memprof_init_bloom(void) {
    _Atomic uint8_t* filter = mmap(NULL, BLOOM_SIZE_BYTES,
                                    PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (filter == MAP_FAILED) return -1;
    
    atomic_store(&g_memprof.bloom_filter_ptr, filter);
    atomic_store(&g_memprof.bloom_ones_count, 0);
    atomic_store(&g_memprof.bloom_rebuild_in_progress, 0);
    return 0;
}
```

---

## 6. Platform Interposition

### 6.1 Linux: LD_PRELOAD Library

**Strategy**: Provide `libspprof_alloc.so` that interposes malloc/free via `LD_PRELOAD`.

**Advantages:**
- No runtime binary patching
- Works with full RELRO (read-only relocations)
- Well-understood, widely used technique
- Can be enabled/disabled at process start

**Disadvantages:**
- Requires environment variable at startup
- Cannot attach to running process

**Implementation:**

```c
/* Resolved via dlsym(RTLD_NEXT, ...) */
static void* (*real_malloc)(size_t) = NULL;
static void* (*real_calloc)(size_t, size_t) = NULL;
static void* (*real_realloc)(void*, size_t) = NULL;
static void  (*real_free)(void*) = NULL;
static int   (*real_posix_memalign)(void**, size_t, size_t) = NULL;
static void* (*real_aligned_alloc)(size_t, size_t) = NULL;
static void* (*real_memalign)(size_t, size_t) = NULL;
static void* (*real_valloc)(size_t) = NULL;
static void* (*real_pvalloc)(size_t) = NULL;

/* 
 * CRITICAL: dlsym RECURSION TRAP
 *
 * On some platforms (Alpine/musl, certain glibc versions), dlsym() itself
 * calls malloc or calloc internally. This creates infinite recursion:
 *   malloc() -> ensure_initialized() -> dlsym() -> calloc() -> ... -> BOOM
 *
 * Solution: Bootstrap heap + initialization guard
 */

/* Bootstrap heap for allocations during dlsym initialization.
 *
 * Size: 64KB (increased from 8KB based on field experience)
 * - Complex dynamic linker paths (many preloaded libraries) can consume
 *   significant memory during symbol resolution
 * - Statically linked binaries in permanent bootstrap mode need more headroom
 * - 64KB is cheap BSS memory (doesn't count against resident memory until touched)
 */
#define BOOTSTRAP_HEAP_SIZE (64 * 1024)
static char bootstrap_heap[BOOTSTRAP_HEAP_SIZE];
static _Atomic size_t bootstrap_offset = 0;
static _Atomic int initializing = 0;

static void* bootstrap_malloc(size_t size) {
    /* Align to 16 bytes */
    size = (size + 15) & ~15UL;
    size_t offset = atomic_fetch_add(&bootstrap_offset, size);
    if (offset + size > BOOTSTRAP_HEAP_SIZE) {
        /* Bootstrap heap exhausted. In permanent bootstrap mode, this is fatal.
         * Return NULL and let caller handle (usually crash, unfortunately). */
        return NULL;
    }
    return &bootstrap_heap[offset];
}

static void* bootstrap_calloc(size_t n, size_t size) {
    void* p = bootstrap_malloc(n * size);
    if (p) memset(p, 0, n * size);
    return p;
}

static void ensure_initialized(void) {
    if (LIKELY(real_malloc != NULL)) return;
    
    /* Prevent recursion: if we're already initializing, use bootstrap */
    if (atomic_exchange(&initializing, 1)) {
        return;  /* Recursive call during init - bootstrap_* will be used */
    }
    
    /* dlsym may call malloc/calloc - those calls will use bootstrap heap */
    real_malloc = dlsym(RTLD_NEXT, "malloc");
    real_calloc = dlsym(RTLD_NEXT, "calloc");
    real_realloc = dlsym(RTLD_NEXT, "realloc");
    real_free = dlsym(RTLD_NEXT, "free");
    real_posix_memalign = dlsym(RTLD_NEXT, "posix_memalign");
    real_aligned_alloc = dlsym(RTLD_NEXT, "aligned_alloc");
    real_memalign = dlsym(RTLD_NEXT, "memalign");
    real_valloc = dlsym(RTLD_NEXT, "valloc");
    real_pvalloc = dlsym(RTLD_NEXT, "pvalloc");
    
    /* 
     * CRITICAL: Handle dlsym failure (static linking, musl edge cases).
     *
     * If real_malloc is NULL after dlsym, we're in an unusual environment
     * (statically linked binary, Alpine musl with broken RTLD_NEXT, etc.).
     *
     * FAIL-FAST vs FAIL-LATE ANALYSIS:
     *
     * Option 1: "Permanent bootstrap mode" (REJECTED)
     *   - Sounds robust: "keep running, just leak memory"
     *   - Reality: 64KB bootstrap heap exhausts in milliseconds
     *   - Result: malloc() returns NULL, app crashes with cryptic SIGSEGV
     *   - This is "Fail Late" - worse than crashing immediately
     *
     * Option 2: abort() with clear error (CHOSEN)
     *   - Immediate, obvious failure
     *   - User knows exactly what went wrong
     *   - No mysterious crashes minutes later
     *   - "Fail Fast" is the correct systems programming approach
     *
     * If you're hitting this in a static binary, the solution is to NOT
     * use LD_PRELOAD - the profiler requires dynamic linking to work.
     */
    if (real_malloc == NULL) {
        /* Write directly to fd 2 (stderr) - fprintf might allocate!
         * 
         * IMPORTANT: Use compile-time constant string with explicit length.
         * strlen() may not be async-signal-safe on all platforms, and we're
         * in a fragile initialization state. sizeof() - 1 is evaluated at
         * compile time and is always safe.
         */
        #define DLSYM_FATAL_MSG \
            "[spprof] FATAL: dlsym(RTLD_NEXT, \"malloc\") returned NULL.\n" \
            "This typically means:\n" \
            "  - The binary is statically linked (LD_PRELOAD won't work)\n" \
            "  - The libc doesn't support RTLD_NEXT properly\n" \
            "  - You're on an unusual platform (musl static, etc.)\n" \
            "\n" \
            "The memory profiler REQUIRES dynamic linking. Aborting.\n"
        write(STDERR_FILENO, DLSYM_FATAL_MSG, sizeof(DLSYM_FATAL_MSG) - 1);
        abort();  /* Fail fast with clear error, not slow death */
        #undef DLSYM_FATAL_MSG
    }
    
    atomic_store(&initializing, 0);
}

/* All allocation functions follow same pattern */
void* malloc(size_t size) {
    /* During initialization, use bootstrap heap (dlsym may call malloc) */
    if (UNLIKELY(atomic_load(&initializing))) {
        return bootstrap_malloc(size);
    }
    
    ensure_initialized();
    /* After ensure_initialized, real_malloc is guaranteed non-NULL
     * (or we've already aborted). No need to check again. */
    
    if (UNLIKELY(tls_state.inside_profiler)) {
        return real_malloc(size);
    }
    
    tls_state.byte_counter -= (int64_t)size;
    
    if (LIKELY(tls_state.byte_counter > 0)) {
        return real_malloc(size);  /* HOT PATH */
    }
    
    /* COLD PATH - sample this allocation */
    tls_state.inside_profiler = 1;
    void* ptr = real_malloc(size);
    if (ptr) handle_sample_alloc(ptr, size);
    tls_state.byte_counter = next_sample_threshold();
    tls_state.inside_profiler = 0;
    return ptr;
}

void* calloc(size_t n, size_t size) {
    /* During initialization, use bootstrap heap */
    if (UNLIKELY(atomic_load(&initializing))) {
        return bootstrap_calloc(n, size);
    }
    ensure_initialized();
    /* ... rest similar to malloc ... */
}

void free(void* ptr) {
    /* Bootstrap allocations cannot be freed (static buffer) */
    if (UNLIKELY(ptr >= (void*)bootstrap_heap && 
                 ptr < (void*)(bootstrap_heap + sizeof(bootstrap_heap)))) {
        return;  /* Silently ignore - bootstrap memory is never reclaimed */
    }
    ensure_initialized();
    /* ... rest of free logic ... */
}
```

**Build:**

```bash
gcc -shared -fPIC -O3 -fno-omit-frame-pointer \
    -o libspprof_alloc.so \
    memprof_interpose_linux.c \
    -ldl -lpthread -lm
```

**Usage:**

```bash
LD_PRELOAD=./libspprof_alloc.so python my_script.py
```

### 6.2 Linux: Runtime GOT Patching (Optional)

For advanced users who need to attach to running processes:

**Strategy**: Parse `/proc/self/maps`, locate GOT entries, atomically swap pointers.

**Challenges:**
- RELRO: Many binaries have read-only GOT after startup
- Race conditions: Must stop-the-world briefly
- Signal safety: Cannot patch while signal handler is running

**Implementation Sketch:**

```c
int memprof_linux_patch_got(void) {
    /* 1. Parse /proc/self/maps to find libc */
    /* 2. Find GOT entries for malloc, free, etc. */
    /* 3. Check if GOT is writable (partial RELRO) */
    /* 4. If full RELRO, use mprotect() to make writable */
    /* 5. Block signals, stop other threads briefly */
    /* 6. Atomic swap of function pointers */
    /* 7. Resume threads, restore signal mask */
    return 0;
}
```

**Recommendation**: Use LD_PRELOAD for production. GOT patching is for research/debugging.

### 6.3 macOS: malloc_logger Callback

**Strategy**: Use Apple's official `malloc_logger` callback mechanism.

**Advantages:**
- Official Apple API (used by Instruments, leaks, heap)
- Captures all zones including custom allocators
- Can be installed/removed at runtime
- No binary patching needed

**Implementation:**

```c
#include <malloc/malloc.h>

/* Apple's callback type */
typedef void (*malloc_logger_t)(uint32_t type, uintptr_t arg1,
                                uintptr_t arg2, uintptr_t arg3,
                                uintptr_t result, uint32_t num_hot_frames);

extern malloc_logger_t malloc_logger;

/*
 * Type bits:
 *   0x01 = allocation
 *   0x02 = deallocation (free)
 *   0x04 = was source of realloc
 *   0x10 = cleared memory (calloc)
 *
 * For allocations: arg2 = size, result = pointer
 * For frees: arg2 = pointer being freed
 */
static void spprof_malloc_logger(uint32_t type, uintptr_t arg1,
                                  uintptr_t arg2, uintptr_t arg3,
                                  uintptr_t result, uint32_t num_hot_frames) {
    (void)arg1; (void)arg3; (void)num_hot_frames;
    
    /* Early exit if being uninstalled (prevents use-after-free during removal) */
    if (atomic_load_explicit(&g_installed_logger, memory_order_acquire) == NULL) {
        return;
    }
    
    if (tls_state.inside_profiler) return;
    ensure_tls_initialized();
    
    if (type & 0x01) {  /* Allocation */
        size_t size = (size_t)arg2;
        void* ptr = (void*)result;
        
        tls_state.byte_counter -= (int64_t)size;
        if (tls_state.byte_counter <= 0) {
            tls_state.inside_profiler = 1;
            handle_sample_alloc(ptr, size);
            tls_state.byte_counter = next_sample_threshold();
            tls_state.inside_profiler = 0;
        }
    }
    
    if (type & 0x02) {  /* Deallocation */
        void* ptr = (void*)arg2;
        /* Note: We check active_free, not active_alloc.
         * This allows stop() to disable new samples while continuing
         * to track frees, preventing "fake leaks" in snapshots. */
        if (ptr && atomic_load(&g_memprof.active_free)) {
            tls_state.inside_profiler = 1;
            handle_free(ptr);
            tls_state.inside_profiler = 0;
        }
    }
}

/*
 * Thread-safe installation using atomic pointer.
 * 
 * CRITICAL: malloc_logger is a global function pointer. Setting it is not
 * atomic, and another thread could be mid-callback during install/uninstall.
 * We use an atomic flag to coordinate installation state.
 */
static _Atomic(malloc_logger_t) g_installed_logger = NULL;

int memprof_darwin_install(void) {
    malloc_logger_t expected = NULL;
    if (!atomic_compare_exchange_strong(&g_installed_logger, 
                                         &expected, 
                                         spprof_malloc_logger)) {
        return -1;  /* Already installed or race condition */
    }
    
    /* Memory fence ensures g_installed_logger is visible before callback */
    atomic_thread_fence(memory_order_seq_cst);
    malloc_logger = spprof_malloc_logger;
    return 0;
}

void memprof_darwin_remove(void) {
    /* Mark as uninstalling first */
    atomic_store(&g_installed_logger, NULL);
    atomic_thread_fence(memory_order_seq_cst);
    
    /* Clear the callback */
    malloc_logger = NULL;
    
    /* Brief delay to let in-flight callbacks complete.
     * Callbacks check g_installed_logger and exit early if NULL.
     * 
     * NOTE: Use nanosleep instead of usleep for POSIX.1-2001 compliance.
     * usleep is not async-signal-safe and is marked obsolete in POSIX.1-2008.
     */
    struct timespec ts = {0, 1000000};  /* 1ms */
    nanosleep(&ts, NULL);
}
```

### 6.4 Windows: Detours Library (EXPERIMENTAL)

⚠️ **STATUS: EXPERIMENTAL** - Windows support is minimal in v1.0. Use at your own risk.

**Strategy**: Use Microsoft Detours to hook CRT allocation functions.

**Advantages:**
- Official Microsoft library
- Well-tested, widely used
- Works with all CRT versions

**Known Limitations (v1.0):**

| Issue | Impact | Status |
|-------|--------|--------|
| Only hooks CRT malloc | Misses HeapAlloc, VirtualAlloc | Not addressed |
| TLS via `__declspec(thread)` | DLL loading caveats on older Windows | Not addressed |
| No realloc/calloc hooks shown | Incomplete coverage | Implementation TODO |
| Thread suspension risks | Detours transaction may deadlock | Needs testing |

**Not tracked in v1.0:**
- `HeapAlloc`/`HeapFree` (used by many native Windows apps)
- `VirtualAlloc`/`VirtualFree` (large allocations bypass CRT)
- `CoTaskMemAlloc` (COM allocations)
- Custom allocators in third-party DLLs

**Recommendation:** For Windows profiling in v1.0, consider using Visual Studio's built-in
heap profiler or ETW (Event Tracing for Windows) instead. Full Windows support is planned
for v1.1+.

**Implementation:**

```c
#include <windows.h> 
#include <detours.h>

typedef void* (__cdecl *malloc_fn)(size_t);
typedef void  (__cdecl *free_fn)(void*);

static malloc_fn Real_malloc = NULL;
static free_fn   Real_free = NULL;

void* __cdecl Hooked_malloc(size_t size) {
    if (tls_state.inside_profiler) {
        return Real_malloc(size);
    }
    
    tls_state.byte_counter -= (LONGLONG)size;
    
    if (tls_state.byte_counter > 0) {
        return Real_malloc(size);
    }
    
    tls_state.inside_profiler = 1;
    void* ptr = Real_malloc(size);
    if (ptr) handle_sample_alloc(ptr, size);
    tls_state.byte_counter = next_sample_threshold();
    tls_state.inside_profiler = 0;
    return ptr;
}

int memprof_windows_install(void) {
    HMODULE ucrt = GetModuleHandleA("ucrtbase.dll");
    if (!ucrt) ucrt = GetModuleHandleA("msvcrt.dll");
    if (!ucrt) return -1;
    
    Real_malloc = (malloc_fn)GetProcAddress(ucrt, "malloc");
    Real_free = (free_fn)GetProcAddress(ucrt, "free");
    
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)Real_malloc, Hooked_malloc);
    DetourAttach(&(PVOID&)Real_free, Hooked_free);
    
    return (DetourTransactionCommit() == NO_ERROR) ? 0 : -1;
}
```

---

## 7. Sampling Engine

### 7.1 TLS Initialization

```c
static __thread MemProfThreadState tls_state = {0};

/* Global seed entropy - read once from /dev/urandom or similar */
static uint64_t g_global_seed = 0;

static void init_global_seed_once(void) {
    static _Atomic int done = 0;
    if (atomic_exchange(&done, 1)) return;  /* Already done */
    
    /* Try /dev/urandom for strong entropy */
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        read(fd, &g_global_seed, sizeof(g_global_seed));
        close(fd);
    } else {
        /* Fallback: mix multiple entropy sources */
        g_global_seed = (uint64_t)time(NULL) * 0x5851F42D4C957F2DULL;
    }
}

static void init_thread_state(void) {
    if (tls_state.initialized) return;
    
    init_global_seed_once();
    
    /* Seed PRNG with thread-unique + process-unique + global entropy.
     * This prevents correlated sequences in:
     *   - Parallel Celery workers starting simultaneously (same time_ns)
     *   - Threads within a process (same pid)
     *   - Fork children (inherit parent's seed, but different pid) */
    uint64_t tid = (uint64_t)(uintptr_t)&tls_state;
    uint64_t time_ns = get_monotonic_ns();
    uint64_t pid = (uint64_t)getpid();
    
    tls_state.prng_state[0] = tid ^ time_ns ^ g_global_seed ^ 0x123456789ABCDEF0ULL;
    tls_state.prng_state[1] = (tid << 32) ^ (time_ns >> 32) ^ (pid << 48) ^ 
                              g_global_seed ^ 0xFEDCBA9876543210ULL;
    
    /* Mix state to avoid correlated initial sequences */
    for (int i = 0; i < 10; i++) {
        (void)prng_next();
    }
    
    /* Set initial sampling threshold */
    uint64_t rate = atomic_load_explicit(&g_memprof.sampling_rate, 
                                         memory_order_relaxed);
    tls_state.byte_counter = (int64_t)next_sample_threshold(rate);
    
    tls_state.inside_profiler = 0;
    tls_state.initialized = 1;
}
```

### 7.2 PRNG Implementation

```c
/**
 * xorshift128+ - Fast, high-quality PRNG
 *
 * Properties:
 *   - Period: 2^128 - 1
 *   - Speed: ~1.5 cycles per call
 *   - Quality: Passes BigCrush
 */
static inline uint64_t prng_next(void) {
    uint64_t s0 = tls_state.prng_state[0];
    uint64_t s1 = tls_state.prng_state[1];
    uint64_t result = s0 + s1;
    
    s1 ^= s0;
    tls_state.prng_state[0] = ((s0 << 55) | (s0 >> 9)) ^ s1 ^ (s1 << 14);
    tls_state.prng_state[1] = (s1 << 36) | (s1 >> 28);
    
    return result;
}

/**
 * Generate uniform double in [0, 1)
 */
static inline double prng_next_double(void) {
    return (prng_next() >> 11) * (1.0 / (1ULL << 53));
}
```

### 7.3 Exponential Threshold Generation

```c
/**
 * Generate next sampling threshold using exponential distribution.
 *
 * Mathematical basis: If X ~ Exponential(λ), then X = -ln(U)/λ
 * where U ~ Uniform(0,1) and λ = 1/mean.
 *
 * @param mean_bytes  Average bytes between samples
 * @return Threshold in bytes (always positive)
 */
static inline int64_t next_sample_threshold(uint64_t mean_bytes) {
    double u = prng_next_double();
    
    /* Clamp to prevent ln(0) and extreme values.
     * 
     * u = 1e-10 → threshold ≈ 23×mean (reasonable upper bound)
     * u = 1e-15 → threshold ≈ 35×mean (excessive, wastes cycles)
     * 
     * We want sampling intervals to stay within ~25× mean for
     * reasonable variance while preventing pathological outliers. */
    if (u < 1e-10) u = 1e-10;
    if (u > 1.0 - 1e-10) u = 1.0 - 1e-10;
    
    double threshold = -((double)mean_bytes) * log(u);
    
    /* Clamp to reasonable range: [1 byte, 1TB] */
    if (threshold < 1.0) threshold = 1.0;
    if (threshold > (double)(1ULL << 40)) threshold = (double)(1ULL << 40);
    
    return (int64_t)threshold;
}
```

### 7.4 Hot Path Analysis

```c
void* malloc_hook(size_t size) {
    /* 
     * HOT PATH - executed for EVERY allocation
     * Must be as fast as possible: ~5-10 cycles
     */
    
    /* Load TLS (1-2 cycles on modern CPUs with __thread) */
    if (UNLIKELY(!tls_state.initialized)) {
        /* Cold path: first allocation on this thread */
        init_thread_state();
    }
    
    /* Re-entrancy check (1 cycle: load + compare) */
    if (UNLIKELY(tls_state.inside_profiler)) {
        return real_malloc(size);
    }
    
    /* Decrement counter (1 cycle: subtract) */
    tls_state.byte_counter -= (int64_t)size;
    
    /* Check if we should sample (1 cycle: compare + branch) */
    if (LIKELY(tls_state.byte_counter > 0)) {
        /* 
         * FAST PATH - ~5 cycles total
         * 99.99% of allocations take this path
         */
        return real_malloc(size);
    }
    
    /*
     * COLD PATH - ~500-2000 cycles
     * Only 0.01% of allocations (at 512KB rate)
     */
    return malloc_cold_path(size);
}
```

---

## 8. Stack Capture and Interning

### 8.1 Native Stack Capture

**Frame Pointer Requirement & Deployment Considerations:**

The memory profiler relies on frame pointer walking for native stack capture. This has important
deployment implications:

| Scenario | Frame Pointers Available? | Recommendation |
|----------|---------------------------|----------------|
| Python itself | ✅ Yes (usually) | Standard Python builds include FP |
| spprof extension | ✅ Yes (we control build) | Always compile with `-fno-omit-frame-pointer` |
| NumPy, SciPy wheels | ❌ Often No | PyPI wheels often omit FP for performance |
| Custom C extensions | ⚠️ Depends | User must compile with `-fno-omit-frame-pointer` |
| System libraries | ⚠️ Varies | Most distros include FP in debug packages |

**Impact of Missing Frame Pointers:**

When a C extension is compiled without frame pointers, the stack walk will stop at that
frame. The allocation is still tracked, but the call stack will be truncated:

```
Expected stack:              Truncated stack (missing FP):
numpy.core.multiarray._sum   numpy.core.multiarray._sum
numpy.core.fromnumeric.sum   <native>+0x1234
my_script.py:process()       [Python frames continue...]
my_script.py:main()
```

**Mitigation Strategies:**

1. **Document requirement clearly**: Users must know to rebuild C extensions with FP

2. **Provide DWARF unwinding fallback** (SIGNIFICANTLY slower, use with caution):

⚠️ **PERFORMANCE WARNING - DWARF UNWINDING**:

| Method | Cost per unwind | Global locks? | Production safe? |
|--------|-----------------|---------------|------------------|
| Frame pointer | ~50-100 cycles | No | ✅ Yes |
| DWARF (libunwind) | ~5,000-50,000 cycles | **Yes** (dl_iterate_phdr) | ⚠️ Risky |

DWARF unwinding via libunwind is **100-1000× slower** than frame pointer walking.
Enabling it will **immediately blow the <0.1% overhead budget**.

Additionally, libunwind calls `dl_iterate_phdr()` which holds a global lock.
If your application uses `dlopen()`/`dlclose()` heavily (plugin systems, JIT),
this can cause lock contention or even deadlocks.

**When to use DWARF fallback:**
- Debug/development only
- Profiling runs where you accept 5-10% overhead
- When you absolutely need stacks through FP-less code

**How to enable** (compile-time only, explicit opt-in required):

```c
/* Compile-time option for DWARF unwinding via libunwind
 * 
 * BUILD: gcc -DMEMPROF_USE_LIBUNWIND -lunwind ...
 * 
 * WARNING: This option increases overhead from <0.1% to potentially 5-10%.
 * Only enable for debugging or when frame pointers are completely unavailable.
 */
#ifdef MEMPROF_USE_LIBUNWIND
#include <libunwind.h>

static int capture_native_stack_dwarf(uintptr_t* frames, int max_depth, int skip) {
    unw_cursor_t cursor;
    unw_context_t context;
    
    unw_getcontext(&context);
    unw_init_local(&cursor, &context);
    
    int depth = 0;
    while (depth < max_depth + skip && unw_step(&cursor) > 0) {
        unw_word_t pc;
        unw_get_reg(&cursor, UNW_REG_IP, &pc);
        if (depth >= skip) {
            frames[depth - skip] = (uintptr_t)pc;
        }
        depth++;
    }
    return (depth > skip) ? (depth - skip) : 0;
}
#endif
```

3. **Runtime detection with user warning**: Detect missing FP and emit one-time warning:

```c
/* Heuristic: Deep Python + shallow native = likely missing frame pointers */
static _Atomic int g_fp_warning_emitted = 0;

void check_frame_pointer_health(int native_depth, int python_depth) {
    /* Suspicious: Deep Python call stack but native stack truncated */
if (native_depth < 3 && python_depth > 5) {
    atomic_fetch_add(&g_memprof.shallow_stack_warnings, 1);
        
        /* Emit one-time warning (first 10 occurrences) */
        int prev = atomic_fetch_add(&g_fp_warning_emitted, 1);
        if (prev < 10) {
            fprintf(stderr,
                "[spprof] WARNING: Native stacks truncated (depth=%d). "
                "C extensions may be compiled without frame pointers.\n"
                "For full stack traces, rebuild extensions with:\n"
                "  CFLAGS='-fno-omit-frame-pointer' pip install --no-binary :all: <package>\n"
                "Or use debug builds of NumPy/SciPy.\n",
                native_depth);
        }
        if (prev == 9) {
            fprintf(stderr, "[spprof] (Suppressing further frame pointer warnings)\n");
        }
    }
}
```

**Python API for diagnostics:**

```python
def get_frame_pointer_health() -> dict:
    """Check if frame pointers are available in loaded extensions."""
    stats = memprof.get_stats()
    return {
        "shallow_stack_warnings": stats.shallow_stack_warnings,
        "likely_missing_fp": stats.shallow_stack_warnings > 10,
        "recommendation": (
            "Rebuild C extensions with -fno-omit-frame-pointer"
            if stats.shallow_stack_warnings > 10 else None
        )
}
```

**Build Instructions for Full Stack Visibility:**

```bash
# Rebuild NumPy with frame pointers (example)
CFLAGS="-fno-omit-frame-pointer" pip install --no-binary numpy numpy

# Or set globally in pip.conf
[global]
build-options = --global-option="build_ext" --global-option="-fno-omit-frame-pointer"
```

---

```c
/**
 * Capture native stack frames via frame pointer walking.
 *
 * CRITICAL: This function must NOT call malloc or any function that might.
 * It uses only stack-allocated data and direct memory reads.
 *
 * Requirements:
 *   - Compiled with -fno-omit-frame-pointer
 *   - Frame pointers present in target code
 *
 * Fallback: If frame pointers unavailable, captures only current PC.
 * Alternative: Define MEMPROF_USE_LIBUNWIND for DWARF-based unwinding.
 *
 * @param frames     Output array for return addresses
 * @param max_depth  Maximum frames to capture
 * @param skip       Frames to skip (exclude profiler frames)
 * @return Number of frames captured
 */
/* Platform-specific user-space address validation.
 *
 * Different architectures and OSes have different user/kernel space splits:
 * - x86_64 Linux:   User space is 0x0000000000000000 - 0x00007FFFFFFFFFFF
 * - x86_64 macOS:   User space is 0x0000000000000000 - 0x00007FFFFFFFFFFF  
 * - ARM64 Linux:    User space is 0x0000000000000000 - 0x0000FFFFFFFFFFFF
 * - ARM64 macOS:    User space is 0x0000000000000000 - 0x0000FFFFFFFFFFFF
 * - i386 Linux:     User space is 0x00000000 - 0xBFFFFFFF (3GB/1GB split)
 *
 * We use conservative upper bounds to avoid false positives.
 */
#if defined(__x86_64__) || defined(_M_X64)
    #define ADDR_MAX_USER   0x00007FFFFFFFFFFFULL
    #define ADDR_ALIGN_MASK 0x7ULL  /* 8-byte alignment */
#elif defined(__aarch64__) || defined(_M_ARM64)
    #define ADDR_MAX_USER   0x0000FFFFFFFFFFFFULL
    #define ADDR_ALIGN_MASK 0x7ULL  /* 8-byte alignment */
#elif defined(__i386__)
    #define ADDR_MAX_USER   0xBFFFFFFFUL
    #define ADDR_ALIGN_MASK 0x3UL   /* 4-byte alignment */
#else
    /* Fallback: disable upper bound check, rely on other validation */
    #define ADDR_MAX_USER   UINTPTR_MAX
    #define ADDR_ALIGN_MASK 0x7ULL
#endif

static inline int capture_native_stack(uintptr_t* frames, int max_depth, int skip) {
    int depth = 0;
    void* fp;
    
    /* Get current frame pointer (architecture-specific) */
#if defined(__x86_64__) || defined(_M_X64)
    __asm__ volatile("mov %%rbp, %0" : "=r"(fp));
#elif defined(__aarch64__) || defined(_M_ARM64)
    __asm__ volatile("mov %0, x29" : "=r"(fp));
#elif defined(__i386__)
    __asm__ volatile("mov %%ebp, %0" : "=r"(fp));
#else
    fp = __builtin_frame_address(0);
#endif

    while (fp && depth < max_depth + skip) {
        uintptr_t fp_val = (uintptr_t)fp;
        
        /* Validate frame pointer using platform-specific bounds */
        if (fp_val < 0x1000) break;                    /* NULL-ish (first page unmapped) */
        if (fp_val > ADDR_MAX_USER) break;             /* Kernel space (platform-specific) */
        if ((fp_val & ADDR_ALIGN_MASK) != 0) break;    /* Misaligned for platform */
        
        /* Read frame: [prev_fp, return_addr] */
        void** frame = (void**)fp;
        void* ret_addr = frame[1];
        void* prev_fp = frame[0];
        
        /* Validate return address */
        if (!ret_addr) break;
        if ((uintptr_t)ret_addr < 0x1000) break;
        
        /* Detect infinite loop (corrupted stack) */
        if ((uintptr_t)prev_fp <= fp_val) break;
        
        /* Store frame if past skip count */
        if (depth >= skip) {
            frames[depth - skip] = (uintptr_t)ret_addr;
        }
        
        depth++;
        fp = prev_fp;
    }
    
    return (depth > skip) ? (depth - skip) : 0;
}
```

### 8.2 Stack Hashing

```c
/**
 * FNV-1a hash for stack frames.
 *
 * Properties:
 *   - Good distribution
 *   - Fast (no divisions)
 *   - Deterministic
 */
static inline uint64_t fnv1a_hash_stack(const uintptr_t* frames, int depth) {
    uint64_t hash = 0xCBF29CE484222325ULL;  /* FNV offset basis */
    
    const uint8_t* data = (const uint8_t*)frames;
    size_t len = depth * sizeof(uintptr_t);
    
    for (size_t i = 0; i < len; i++) {
        hash ^= data[i];
        hash *= 0x100000001B3ULL;  /* FNV prime */
    }
    
    return hash;
}
```

### 8.3 Mixed-Mode Stack Capture (Python + Native)

**Key Design Decision**: Users want to see `my_script.py:50` in allocation stacks, not just `libpython.so+0x123`. We achieve this by capturing **both** Python frames and native frames, then merging them during resolution.

**Capture Strategy:**

```c
/**
 * capture_mixed_stack() - Capture Python and native frames together
 *
 * This function captures a unified stack trace containing both:
 *   1. Python frames (function name, filename, line) - via existing framewalker.c
 *   2. Native frames (return addresses) - via frame pointer walking
 *
 * The merge follows the "Trim & Sandwich" algorithm from the CPU profiler:
 *   - Walk native stack from leaf
 *   - When we hit PyEval_EvalFrameDefault (or similar), insert Python frames
 *   - Continue with remaining native frames
 *
 * CRITICAL: This function is called in the sampling cold path, not hot path.
 * It can use the existing framewalker infrastructure since we're inside
 * the re-entrancy guard (inside_profiler = 1).
 */
typedef struct {
    uintptr_t native_pcs[MEMPROF_MAX_STACK_DEPTH];
    int native_depth;
    uintptr_t python_code_ptrs[MEMPROF_MAX_STACK_DEPTH];
    int python_depth;
} MixedStackCapture;

static int capture_mixed_stack(MixedStackCapture* out) {
    /* 1. Capture native frames (fast, no allocations) */
    out->native_depth = capture_native_stack(out->native_pcs, MEMPROF_MAX_STACK_DEPTH, 3);
    
    /* 2. Capture Python frames using existing framewalker.c infrastructure
     *    This reuses the same code as the CPU profiler, ensuring consistency.
     *    The framewalker handles Python version differences (3.11-3.14). */
    out->python_depth = framewalker_capture_raw(out->python_code_ptrs, MEMPROF_MAX_STACK_DEPTH);
    
    return out->native_depth + out->python_depth;
}
```

**Resolution Strategy (Async):**

During symbol resolution, we merge the stacks:

```c
/**
 * is_python_interpreter_frame() - Detect if a frame is inside Python interpreter core
 *
 * This heuristic determines whether a native frame belongs to the Python interpreter
 * itself (where we should insert Python frames) vs. C extension code (which we keep).
 *
 * Detection strategy:
 * 1. Check if the shared object name contains "python" (libpython3.11.so, python311.dll)
 * 2. Check if function name matches interpreter patterns (PyEval_*, _PyEval_*, PyObject_*)
 *
 * KNOWN LIMITATIONS:
 * - Cython-generated .so files may be misclassified (acceptable - appear as native frames)
 * - PyPy has different interpreter structure (would need separate detection)
 * - Embedded Python in C++ apps may have unusual library names
 *
 * False positives (Cython .so): Acceptable - they appear as native frames
 * False negatives (unusual Python builds): May result in duplicated frames
 *
 * @param dli_fname  Shared object path from dladdr()
 * @param dli_sname  Symbol name from dladdr() (may be NULL)
 * @return 1 if this appears to be a Python interpreter frame, 0 otherwise
 */
static int is_python_interpreter_frame(const char* dli_fname, const char* dli_sname) {
    /* Check shared object name for "python" */
    if (dli_fname) {
        /* Match: libpython3.11.so, python311.dll, Python.framework, etc. */
        if (strstr(dli_fname, "python") || strstr(dli_fname, "Python")) {
            /* Verify it's the interpreter, not a C extension with "python" in name */
            if (dli_sname) {
                /* Core interpreter functions we want to skip */
                if (strncmp(dli_sname, "PyEval_", 7) == 0 ||
                    strncmp(dli_sname, "_PyEval_", 8) == 0 ||
                    strncmp(dli_sname, "PyObject_", 9) == 0 ||
                    strncmp(dli_sname, "_PyObject_", 10) == 0 ||
                    strncmp(dli_sname, "PyFrame_", 8) == 0 ||
                    strcmp(dli_sname, "pymain_run_python") == 0 ||
                    strcmp(dli_sname, "Py_RunMain") == 0) {
                    return 1;
                }
            }
            /* No symbol name but in Python library - likely interpreter */
            if (!dli_sname) return 1;
        }
    }
    return 0;
}

/**
 * resolve_mixed_stack() - Merge Python and native frames into coherent trace
 *
 * Called from async resolver, NOT during capture. Can use dladdr, Python API.
 *
 * Output order (leaf to root):
 *   1. Native frames until we hit Python interpreter
 *   2. Python frames (most useful for users)
 *   3. Remaining native frames (Python startup, etc.)
 */
int resolve_mixed_stack(const MixedStackCapture* capture, ResolvedFrame* out_frames) {
    int out_idx = 0;
    int python_inserted = 0;
    
    for (int i = 0; i < capture->native_depth && out_idx < MAX_RESOLVED; i++) {
        Dl_info info;
        if (dladdr((void*)capture->native_pcs[i], &info)) {
            int is_interpreter = is_python_interpreter_frame(info.dli_fname, info.dli_sname);
            
            if (is_interpreter && !python_inserted) {
                /* Insert Python frames here */
                for (int j = 0; j < capture->python_depth && out_idx < MAX_RESOLVED; j++) {
                    resolve_python_code_object(capture->python_code_ptrs[j], &out_frames[out_idx++]);
                }
                python_inserted = 1;
                /* Skip interpreter frames */
            } else if (!is_interpreter) {
                /* Include non-interpreter native frame */
                out_frames[out_idx++] = make_native_frame(&info);
            }
        }
    }
    
    return out_idx;
}
```

**Benefits of Mixed-Mode Approach:**

| Aspect | Benefit |
|--------|---------|
| **Python attribution** | See `numpy.array()` instead of `PyObject_Call` |
| **Native visibility** | See C extension functions that allocate |
| **Consistency** | Same framewalker as CPU profiler |
| **Line accuracy** | Python frames include line numbers |

---

### 8.4 Stack Interning

```c
/**
 * Intern a stack trace, returning a unique 32-bit ID.
 *
 * Lock-free algorithm using CAS on hash field.
 * Concurrent calls may both insert the same stack (harmless duplicate).
 *
 * @param frames  Array of return addresses
 * @param depth   Number of frames
 * @param hash    Pre-computed hash
 * @return Stack ID (index into table), or UINT32_MAX if full
 */
uint32_t stack_table_intern(const uintptr_t* frames, int depth, uint64_t hash) {
    uint64_t idx = hash & (MEMPROF_STACK_TABLE_CAPACITY - 1);
    
    for (int probe = 0; probe < 64; probe++) {
        StackEntry* entry = &g_memprof.stack_table[idx];
        uint64_t entry_hash = atomic_load_explicit(&entry->hash, 
                                                    memory_order_relaxed);
        
        /* Empty slot? Try to claim it */
        if (entry_hash == 0) {
            uint64_t expected = 0;
            if (atomic_compare_exchange_strong_explicit(
                    &entry->hash, &expected, hash,
                    memory_order_acq_rel, memory_order_relaxed)) {
                /* Claimed. Fill in frames */
                entry->depth = (uint16_t)depth;
                entry->flags = 0;
                memcpy(entry->frames, frames, depth * sizeof(uintptr_t));
                atomic_fetch_add(&g_memprof.stack_count, 1);
                return (uint32_t)idx;
            }
            /* Lost race, re-read hash */
            entry_hash = atomic_load_explicit(&entry->hash, memory_order_relaxed);
        }
        
        /* Check if this is our stack */
        if (entry_hash == hash && entry->depth == depth) {
            /* Probable match - verify frames */
            if (memcmp(entry->frames, frames, depth * sizeof(uintptr_t)) == 0) {
                return (uint32_t)idx;  /* Exact match */
            }
        }
        
        /* Collision - linear probe */
        atomic_fetch_add(&g_memprof.stack_table_collisions, 1);
        idx = (idx + 1) & (MEMPROF_STACK_TABLE_CAPACITY - 1);
    }
    
    /* Table full or excessive collisions */
    return UINT32_MAX;
}
```

---

## 9. Python Integration

### 9.1 Module Structure

```
spprof/
├── __init__.py          # Existing: CPU profiler
├── memprof.py           # New: Python wrapper for memory profiler
└── _ext/
    └── memprof/
        ├── memprof.h
        ├── memprof.c
        ├── heap_map.c
        ├── stack_intern.c
        ├── stack_capture.c
        ├── sampling.c
        ├── python_bindings.c
        └── interpose/
            ├── linux.c
            ├── darwin.c
            └── windows.c
```

### 9.2 Python API

```python
# spprof/memprof.py

from __future__ import annotations
from dataclasses import dataclass
from typing import List, Dict, Optional
from pathlib import Path

@dataclass
class AllocationSample:
    """A single sampled allocation."""
    address: int
    size: int
    weight: int
    estimated_bytes: int  # size * weight
    timestamp_ns: int
    lifetime_ns: Optional[int]  # None if still live
    stack: List[StackFrame]

@dataclass  
class StackFrame:
    """A frame in the allocation call stack."""
    address: int
    function: str
    file: str
    line: int
    is_python: bool

@dataclass
class HeapSnapshot:
    """Snapshot of live (unfreed) sampled allocations."""
    samples: List[AllocationSample]
    total_samples: int
    live_samples: int
    estimated_heap_bytes: int
    timestamp_ns: int
    
    # Frame pointer health metrics (for data quality assessment)
    frame_pointer_health: FramePointerHealth
    
    def top_allocators(self, n: int = 10) -> List[Dict]:
        """Get top N allocation sites by estimated bytes."""
        ...
    
    def save(self, path: Path, format: str = "speedscope") -> None: 
        """Save snapshot to file."""
        ...

@dataclass
class FramePointerHealth:
    """
    Metrics for assessing native stack capture quality.
    
    Analysis tools can use this to flag "Low Confidence" profiles where
    frame pointers appear to be missing from C extensions.
    """
    shallow_stack_warnings: int      # Stacks truncated due to missing FP
    total_native_stacks: int         # Total native stacks captured
    avg_native_depth: float          # Average native stack depth
    min_native_depth: int            # Minimum observed depth
    truncation_rate: float           # shallow_stack_warnings / total_native_stacks
    
    @property
    def confidence(self) -> str:
        """
        Human-readable confidence level for profile data.
        
        Returns:
            'high': <5% truncation, good frame pointer coverage
            'medium': 5-20% truncation, some extensions missing FP
            'low': >20% truncation, many extensions missing FP
        """
        if self.truncation_rate < 0.05:
            return 'high'
        elif self.truncation_rate < 0.20:
            return 'medium'
        else:
            return 'low'
    
    @property
    def recommendation(self) -> Optional[str]:
        """Action recommendation if confidence is not high."""
        if self.confidence == 'high':
            return None
        return (
            f"Stack truncation rate is {self.truncation_rate:.1%}. "
            f"For better visibility, rebuild C extensions with: "
            f"CFLAGS='-fno-omit-frame-pointer' pip install --no-binary :all: <package>"
        )

@dataclass
class MemProfStats:
    """Profiler statistics."""
    total_samples: int
    live_samples: int
    freed_samples: int
    unique_stacks: int
    estimated_heap_bytes: int
    heap_map_load_percent: float
    collisions: int
    sampling_rate_bytes: int

# Core API

def start(sampling_rate_kb: int = 512) -> None:
    """
    Start memory profiling.
    
    Args:
        sampling_rate_kb: Average KB between samples. Lower = more accuracy,
                         higher overhead. Default 512 KB gives <0.1% overhead.
    
    Raises:
        RuntimeError: If memory profiler is already running.
        RuntimeError: If interposition hooks could not be installed.
    
    Example:
        >>> import spprof.memprof as memprof
        >>> memprof.start(sampling_rate_kb=256)  # More accurate
        >>> # ... run workload ...
        >>> snapshot = memprof.get_snapshot()
    """
    ...

def stop() -> None:
    """
    Stop memory profiling.
    
    **Important Semantics:**
    - Stops tracking NEW allocations (malloc sampling disabled)
    - CONTINUES tracking frees (free lookup remains active)
    - This prevents "fake leaks" where objects allocated during profiling
      but freed after stop() would incorrectly appear as live
    
    Collected data is preserved until shutdown().
    
    To fully disable all hooks, call shutdown() instead.
    
    Raises:
        RuntimeError: If memory profiler is not running.
    """
    ...

def get_snapshot() -> HeapSnapshot:
    """
    Get snapshot of currently live (unfreed) sampled allocations.
    
    Can be called while profiling is active or after stop().
    
    Returns:
        HeapSnapshot containing all live sampled allocations.
    """
    ...

def get_stats() -> MemProfStats:
    """
    Get profiler statistics.
    
    Returns:
        MemProfStats with current profiler state.
    """
    ...

def shutdown() -> None:
    """
    Shutdown profiler and prepare for process exit.
    
    ⚠️ WARNING: This is a ONE-WAY operation.
    
    - Disables all hooks (no more sampling or free tracking)
    - Does NOT free internal memory (intentional, prevents crashes)
    - Should only be called at process exit or before unloading the module
    
    The "leaked" memory (~30-60MB) will be reclaimed by the OS on process exit.
    This is safer than risking use-after-free crashes from in-flight hooks.
    
    For typical usage (profiling then exiting), this is automatic via atexit.
    You only need to call this explicitly if you're unloading the module
    dynamically or want to ensure clean state for testing.
    
    After shutdown(), calling start() again raises RuntimeError.
    """
    ...

# Profiler Lifecycle State Machine

"""
The memory profiler has well-defined lifecycle states. Understanding these
states helps avoid common usage errors.

LIFECYCLE STATES:
                                    
  UNINITIALIZED ─────[init()]─────► INITIALIZED
        │                                │
        │                                │
        │                          [start()]
        │                                │
        │                                ▼
        │                            ACTIVE
        │                                │
        │                          [stop()]
        │                                │
        │                                ▼
        │                            STOPPED
        │                           /       \\
        │                    [start()]    [shutdown()]
        │                         │            │
        │                         ▼            ▼
        │                      ACTIVE     TERMINATED
        │                                      │
        │                                [start()] → RuntimeError
        │                                      │
        └──────────────────────────────────────┘

STATE DESCRIPTIONS:

| State | Description | Allowed Operations |
|-------|-------------|-------------------|
| UNINITIALIZED | Library loaded but not configured | init() only |
| INITIALIZED | Data structures allocated | start(), shutdown() |
| ACTIVE | Profiling enabled, hooks installed | stop(), get_snapshot(), get_stats() |
| STOPPED | Profiling paused, frees still tracked | start(), get_snapshot(), shutdown() |
| TERMINATED | Hooks disabled, one-way | None (RuntimeError on start()) |

KEY BEHAVIORS:

1. **start() → stop() → start()**: ALLOWED
   - Useful for profiling specific code sections
   - Data accumulates across start/stop cycles
   - Call get_snapshot() before stop() to get just that section's data

2. **stop() continues tracking frees**: INTENTIONAL
   - Prevents "fake leaks" where objects freed after stop() appear as live
   - Frees are tracked until shutdown()

3. **shutdown() is ONE-WAY**: INTENTIONAL
   - Cannot restart profiler after shutdown
   - Raises RuntimeError if start() called after shutdown()
   - Memory is intentionally not freed (safety over cleanliness)

4. **Multiple init() calls**: IGNORED (idempotent)
   - Safe to call init() multiple times
   - Subsequent calls are no-ops if already initialized
"""

# Context manager

class MemoryProfiler:
    """
    Context manager for memory profiling.
    
    Example:
        >>> with MemoryProfiler(sampling_rate_kb=512) as mp:
        ...     # ... run workload ...
        >>> mp.snapshot.save("memory_profile.json")
    """
    
    def __init__(self, sampling_rate_kb: int = 512):
        self._sampling_rate_kb = sampling_rate_kb
        self._snapshot: Optional[HeapSnapshot] = None
    
    def __enter__(self) -> MemoryProfiler:
        start(sampling_rate_kb=self._sampling_rate_kb)
        return self
    
    def __exit__(self, *args) -> None:
        self._snapshot = get_snapshot()
        stop()
    
    @property
    def snapshot(self) -> Optional[HeapSnapshot]:
        return self._snapshot
```

### 9.3 Usage Examples

```python
# Basic usage
import spprof.memprof as memprof

memprof.start(sampling_rate_kb=512)

# ... application code with memory allocations ...
import numpy as np
data = np.random.randn(10000, 10000)  # ~800MB allocation

snapshot = memprof.get_snapshot()
print(f"Estimated heap: {snapshot.estimated_heap_bytes / 1e9:.2f} GB")
print(f"Live samples: {snapshot.live_samples}")

# Top allocators
for site in snapshot.top_allocators(5):
    print(f"{site['function']}: {site['estimated_bytes'] / 1e6:.1f} MB")

memprof.stop()

# Context manager usage
with memprof.MemoryProfiler() as mp:
    # ... workload ...
    pass

mp.snapshot.save("memory_profile.json", format="speedscope")
```

### 9.4 Combined CPU + Memory Profiling

```python
import spprof
import spprof.memprof as memprof

# Both profilers can run simultaneously
spprof.start(interval_ms=10)
memprof.start(sampling_rate_kb=512)

# ... workload ...

cpu_profile = spprof.stop()
mem_snapshot = memprof.get_snapshot()
memprof.stop()

# Save both
cpu_profile.save("cpu_profile.json")
mem_snapshot.save("mem_profile.json")
```

### 9.5 Python GC Epoch Tracking (Optional)

**Problem**: The memory profiler intercepts `malloc/free`, but Python's garbage collector doesn't necessarily call `free()` for every collected object. The GC may:
1. Free objects in bulk cycles
2. Reuse memory internally without returning to malloc
3. Delay deallocation via reference cycles

**Solution**: Optional audit hook integration tracks GC boundaries:

```c
/**
 * GC epoch tracking via sys.addaudithook()
 *
 * When enabled, we increment a global epoch counter on each gc.collect
 * event. This allows analysis tools to correlate allocation lifetimes
 * with GC generations and identify memory that survives multiple cycles.
 */
static _Atomic uint64_t g_gc_epoch = 0;

/* Audit hook callback - registered via PySys_AddAuditHook */
static int gc_audit_hook(const char *event, PyObject *args, void *data) {
    (void)args; (void)data;
    
    if (strcmp(event, "gc.start") == 0) {
        /* GC cycle starting - record current epoch in samples */
        atomic_fetch_add_explicit(&g_gc_epoch, 1, memory_order_relaxed);
    }
    return 0;  /* Allow event to proceed */
}

/* Enable GC tracking (call from memprof_init if desired) */
int memprof_enable_gc_tracking(void) {
    return PySys_AddAuditHook(gc_audit_hook, NULL);
}

/* Get current GC epoch for inclusion in allocation metadata */
uint64_t memprof_get_gc_epoch(void) {
    return atomic_load_explicit(&g_gc_epoch, memory_order_relaxed);
}
```

**Python API Extension:**

```python
@dataclass
class AllocationSample:
    # ... existing fields ...
    gc_epoch: int  # GC epoch when allocated (for generational analysis)
    
@dataclass
class HeapSnapshot:
    # ... existing fields ...
    gc_epochs_during_profile: int  # Number of GC cycles during profiling
    
    def allocations_surviving_gc(self, min_epochs: int = 1) -> List[AllocationSample]:
        """Get allocations that survived at least N GC epochs."""
        current_epoch = _get_current_gc_epoch()
        return [s for s in self.samples 
                if s.gc_epoch + min_epochs <= current_epoch]
```

**Use Case**: Identifying potential memory leaks:
```python
with memprof.MemoryProfiler(gc_tracking=True) as mp:
    for i in range(1000):
        process_batch()
        gc.collect()  # Force GC to update epochs

# Find allocations that survived 3+ GC cycles
survivors = mp.snapshot.allocations_surviving_gc(min_epochs=3)
for alloc in survivors[:10]:
    print(f"Potential leak: {alloc.estimated_bytes}B at {alloc.stack[0]}")
```

**Note**: GC tracking adds minimal overhead (~1µs per GC cycle) and is optional.

---

## 10. API Reference

### 10.1 C API

```c
/**
 * Initialize the memory profiler.
 *
 * Allocates data structures (heap map, stack table) using mmap.
 * Must be called before start().
 *
 * Thread safety: NOT thread-safe. Call once from main thread.
 *
 * @param sampling_rate  Average bytes between samples
 * @return 0 on success, -1 on error (sets errno)
 */
int memprof_init(uint64_t sampling_rate);

/**
 * Start memory profiling.
 *
 * Installs platform-specific interposition hooks.
 *
 * Thread safety: Thread-safe. Can be called from any thread.
 *
 * @return 0 on success, -1 if already running or not initialized
 */
int memprof_start(void);

/**
 * Stop memory profiling.
 *
 * Stops tracking new allocations but CONTINUES tracking frees.
 * This ensures allocations made during profiling are correctly marked
 * as freed if they're deallocated after stop() is called.
 *
 * Collected data is preserved until shutdown().
 *
 * @return 0 on success, -1 if not running
 */
int memprof_stop(void);

/**
 * Get snapshot of live allocations.
 *
 * Allocates output array - caller must call memprof_free_snapshot().
 *
 * @param out_entries  Output: array of heap entries
 * @param out_count    Output: number of entries
 * @return 0 on success, -1 on error
 */
int memprof_get_snapshot(HeapMapEntry** out_entries, size_t* out_count);

/**
 * Free a snapshot returned by memprof_get_snapshot().
 */
void memprof_free_snapshot(HeapMapEntry* entries);

/**
 * Get profiler statistics.
 */
int memprof_get_stats(MemProfStats* out);

/**
 * Resolve symbols for all captured stacks.
 *
 * Should be called after stop() for final report.
 * Uses dladdr/DbgHelp - NOT async-signal-safe.
 *
 * @return Number of stacks resolved
 */
int memprof_resolve_symbols(void);

/**
 * Shutdown profiler and free all resources.
 *
 * ⚠️ CRITICAL SAFETY WARNING:
 *
 * shutdown() is a ONE-WAY DOOR. Once called:
 * 1. Hooks are disabled (g_memprof.active_* = 0)
 * 2. Data structures are NEVER freed if the process continues
 *
 * WHY WE DON'T MUNMAP:
 * If we munmap the heap_map while hooks are still potentially running
 * (thread was sleeping, signal handler interrupted, etc.), the next
 * hook invocation will SEGFAULT dereferencing freed memory.
 *
 * SAFE USAGE PATTERNS:
 *   - Call shutdown() only at process exit (atexit handler)
 *   - Call shutdown() before dlclose() of the profiler library
 *   - Let OS reclaim memory on process termination
 *
 * UNSAFE:
 *   - Calling shutdown() then continuing to run the application
 *   - Calling shutdown() from a signal handler
 *   - Unloading libspprof_alloc.so without calling shutdown()
 *
 * The "leaked" memory on shutdown is intentional. For a typical profiling
 * session, this is ~30-60MB that will be reclaimed by the OS on exit.
 */
void memprof_shutdown(void);
```

### 10.2 Heap Map API

```c
/**
 * Initialize the heap map.
 *
 * Uses mmap to allocate backing array (avoids malloc recursion).
 *
 * @return 0 on success, -1 on error
 */
int heap_map_init(void);

/**
 * Insert a sampled allocation.
 *
 * Lock-free via CAS. May fail if table is full.
 *
 * @return 1 if inserted, 0 if table full
 */
int heap_map_insert(uintptr_t ptr, uint32_t stack_id, 
                    uint32_t size, uint32_t weight, uint64_t timestamp);

/**
 * Look up and remove a freed allocation.
 *
 * Lock-free. Fast path for non-sampled allocations.
 *
 * @return 1 if found and removed, 0 if not found
 */
int heap_map_remove(uintptr_t ptr, uint32_t* out_stack_id,
                    uint32_t* out_size, uint32_t* out_weight,
                    uint64_t* out_duration);

/**
 * Get current load factor.
 *
 * @return Load factor as percentage (0-100)
 */
int heap_map_load_percent(void);

/**
 * Free heap map resources.
 */
void heap_map_destroy(void);
```

---

## 11. Performance Characteristics

### 11.1 Overhead Analysis

| Path | Operations | Cycles | Time |
|------|-----------|--------|------|
| Fast path (no sample) | TLS load, subtract, branch | 5-10 | ~2ns |
| Cold path (sample) | Stack walk, hash, reserve, insert | 500-2000 | ~500ns |
| Free (not sampled) | **Bloom filter check** | 8-12 | **~3ns** |
| Free (Bloom false positive) | Bloom + heap map probe | 30-50 | ~15ns |
| Free (sampled) | Bloom + hash, probe, delete | 40-70 | ~25ns |

**Note**: Bloom filter reduces free() overhead by ~5× for non-sampled allocations (99.99% of frees).

### 11.2 Expected Overhead by Sampling Rate

| Sampling Rate | Samples/sec* | Cold Path % | Total Overhead |
|--------------|--------------|-------------|----------------|
| 64 KB | ~1600 | 0.16% | ~0.8% CPU |
| 128 KB | ~800 | 0.08% | ~0.4% CPU |
| 256 KB | ~400 | 0.04% | ~0.2% CPU |
| **512 KB** (default) | ~200 | 0.02% | **~0.1% CPU** |
| 1 MB | ~100 | 0.01% | ~0.05% CPU |
| 4 MB | ~25 | 0.0025% | ~0.01% CPU |

*Assumes 100 MB/s allocation rate, 1M allocations/sec

### 11.3 Memory Usage

| Component | Initial | Maximum | Notes |
|-----------|---------|---------|-------|
| Heap Map | 24 MB | 24 MB | 1M entries × 24 bytes (fixed) |
| Stack Table | **~2 MB** | **~35 MB** | 4K→64K entries, grows dynamically |
| Bloom Filter | 128 KB | 128 KB | 1M bits for free() optimization |
| TLS per thread | 1 KB | 1 KB | Pre-allocated frame buffer |
| **Total** | **~27 MB** | **~60 MB** | Grows with unique stack count |

**Memory improvements (v1.0.3):**
- Dynamic stack table: Start at 2MB instead of 140MB (70× reduction)
- Packed metadata: 16MB saved (40B → 24B per entry)
- Bloom filter via mmap: Uniform allocation, cleaner cleanup

**Tuning for large applications:**
```bash
# For apps with many unique call stacks (JIT-heavy, deep recursion)
export SPPROF_STACK_TABLE_MAX=262144  # 256K entries, ~140MB max
```

### 11.4 Benchmark Results (Expected)

```
Benchmark: numpy matrix multiplication (10000x10000)
Without profiler:        2.34s
With memprof (512KB):    2.35s (+0.4%)
With memprof (64KB):     2.41s (+3.0%)

Benchmark: allocation stress (1M allocs/sec)
Without profiler:        1.00x baseline
With memprof (512KB):    1.001x (+0.1%)
With memprof (64KB):     1.008x (+0.8%)
```

---

## 12. Safety and Correctness

### 12.1 Re-entrancy Safety

**Problem**: Profiler code might call malloc, causing infinite recursion.

**Solution**: Thread-local `inside_profiler` guard.

```c
void* malloc_hook(size_t size) {
    if (tls_state.inside_profiler) {
        return real_malloc(size);  /* Bypass profiler */
    }
    
    if (should_sample(size)) {
        tls_state.inside_profiler = 1;  /* Set guard BEFORE any work */
        /* Now safe to call functions that might allocate */
        capture_stack();
        intern_stack();
        insert_heap_map();
        tls_state.inside_profiler = 0;
    }
    
    return real_malloc(size);
}
```

### 12.2 Signal Safety

**Problem**: Stack capture might be interrupted by signal.

**Solution**: 
1. No locks in capture path
2. Atomic operations only
3. Pre-allocated buffers
4. Frame pointer walk is inherently async-signal-safe

### 12.3 Thread Safety

| Operation | Mechanism |
|-----------|-----------|
| TLS access | Thread-local storage (no sync needed) |
| Heap map reserve | CAS on ptr field (EMPTY → RESERVED) |
| Heap map finalize | Atomic store of packed metadata, then ptr |
| Heap map remove | Relaxed store (only one thread can own a ptr) |
| Heap map snapshot | Acquire loads on ptr + metadata (atomic consistency) |
| Stack intern | CAS on hash field |
| Bloom filter | Atomic OR (add) / relaxed load (check) |
| Statistics | Atomic increment |

**Two-Phase Insert & "Death During Birth" State Machine:**

The reserve→finalize pattern with wait-free handling ensures correctness without spinning:

```
NORMAL ALLOCATION:                    DEATH DURING BIRTH:
Thread 1 (malloc):                    Thread 1 (malloc):        Thread 2 (free):
1. ptr = real_malloc()                1. ptr = real_malloc()
2. heap_map_reserve(ptr)              2. heap_map_reserve(ptr)
   [CAS: EMPTY → RESERVED]               [CAS: EMPTY → RESERVED]
3. capture_stack()                    3. capture_stack()        4. free(ptr) called
4. heap_map_finalize(ptr)                                          → sees RESERVED
   [CAS: RESERVED → ptr] ✓                                         → CAS: RESERVED → TOMBSTONE ✓
                                      5. heap_map_finalize(ptr)
                                         [CAS: RESERVED → ptr] FAILS
                                         → sees TOMBSTONE
                                         → cleans up, returns 0
```

**State Transition Diagram:**

```
     ┌─────────────────────────────────────────────────────────┐
     │                                                         │
     ▼                                                         │
  EMPTY ──────► RESERVED ──────► ptr (OCCUPIED) ──────► TOMBSTONE
    ▲              │                    │                   │
    │              │                    │                   │
    │              └────────────────────┼───────────────────┤
    │               (death during       │ (normal free)     │
    │                birth)             │                   │
    └───────────────────────────────────┴───────────────────┘
                        (compaction)
```

**Key Invariants:**
1. Only malloc can transition EMPTY → RESERVED (CAS)
2. Only malloc can transition RESERVED → ptr (CAS, may fail)
3. Only free can transition RESERVED → TOMBSTONE or ptr → TOMBSTONE
4. Tombstone reuse: malloc can transition TOMBSTONE → RESERVED (CAS)
5. Compaction (optional background) can transition TOMBSTONE → EMPTY

### 12.3.1 Tombstone Accumulation and Compaction Strategy

**Problem**: Linear probing degrades when tombstones accumulate. Even with tombstone reuse
during insertion, lookups for old pointers may traverse long probe sequences filled with
tombstones, violating the O(1) lookup guarantee.

**Analysis - Why Tombstone Reuse Isn't Sufficient:**

Tombstone reuse (TOMBSTONE → RESERVED in `heap_map_reserve`) helps insertions find slots,
but doesn't help lookups for pointers that hash to regions with many tombstones:

```
Scenario: Region with probe sequence [T, T, T, T, T, P, E, E, E, E]
          where T=TOMBSTONE, P=occupied ptr, E=EMPTY

Insert new_ptr: Finds slot at index 0 (T→RESERVED), O(1) ✓
Lookup old_ptr: Must probe through T, T, T, T, T to find P or E, O(5) ✗
```

**Bounded Accumulation Proof:**

Tombstones are bounded because:
1. Each sampled allocation creates at most 1 tombstone (when freed)
2. Tombstone reuse converts TOMBSTONE → RESERVED, removing the tombstone
3. Maximum tombstones ≤ (heap_map_capacity - live_entries)

**Worst case**: All 1M slots contain tombstones → no live entries → nothing to look up!
**Realistic worst case**: 900K tombstones, 100K live → average probe length ~10

**Compaction Strategy (Optional, Background):**

For long-running processes where tombstone clustering degrades performance, an optional
background compaction can consolidate entries:

```c
/**
 * heap_map_compact_region() - Defragment a region of the heap map
 *
 * This is OPTIONAL and only needed for processes running weeks/months with
 * high allocation churn in specific hot regions.
 *
 * Algorithm (Robin Hood-inspired):
 * 1. Scan region for OCCUPIED entries displaced far from home position
 * 2. For each displaced entry, check if a closer slot is TOMBSTONE
 * 3. CAS-move entry closer to home, leaving TOMBSTONE at old position
 * 4. Once a contiguous TOMBSTONE run exists, CAS them to EMPTY
 *
 * Complexity: O(region_size) per call, amortized O(1) per entry moved
 * Frequency: Trigger when avg_probe_length > threshold (e.g., 16)
 *
 * CRITICAL: Must be lock-free and tolerate concurrent operations.
 */
int heap_map_compact_region(uint64_t start_idx, uint64_t count) {
    uint64_t compacted = 0;
    
    for (uint64_t i = 0; i < count; i++) {
        uint64_t idx = (start_idx + i) & MEMPROF_HEAP_MAP_MASK;
        HeapMapEntry* entry = &g_memprof.heap_map[idx];
        
        uintptr_t ptr = atomic_load_explicit(&entry->ptr, memory_order_acquire);
        
        /* Skip non-occupied entries */
        if (ptr == HEAP_ENTRY_EMPTY || ptr == HEAP_ENTRY_TOMBSTONE ||
            ptr == HEAP_ENTRY_RESERVED) {
            continue;
        }
        
        /* Calculate home position and displacement */
        uint64_t home = hash_ptr(ptr) & MEMPROF_HEAP_MAP_MASK;
        int64_t displacement = (int64_t)(idx - home);
        if (displacement < 0) displacement += MEMPROF_HEAP_MAP_CAPACITY;
        
        /* Try to move closer to home if displacement > threshold */
        if (displacement > 4) {
            /* Look for TOMBSTONE slot between home and current position */
            for (int64_t d = 0; d < displacement; d++) {
                uint64_t target = (home + d) & MEMPROF_HEAP_MAP_MASK;
                HeapMapEntry* target_entry = &g_memprof.heap_map[target];
                
                uintptr_t target_ptr = atomic_load_explicit(&target_entry->ptr,
                                                            memory_order_relaxed);
                if (target_ptr == HEAP_ENTRY_TOMBSTONE) {
                    /* Try to move: CAS target TOMBSTONE → RESERVED */
                    if (atomic_compare_exchange_strong(&target_entry->ptr,
                            &target_ptr, HEAP_ENTRY_RESERVED)) {
                        /* Copy metadata to target */
                        uint64_t metadata = atomic_load(&entry->metadata);
                        uint64_t timestamp = atomic_load(&entry->timestamp);
                        uint64_t birth_seq = atomic_load(&entry->birth_seq);
                        
                        atomic_store(&target_entry->metadata, metadata);
                        atomic_store(&target_entry->timestamp, timestamp);
                        atomic_store(&target_entry->birth_seq, birth_seq);
                        
                        /* Publish: target RESERVED → ptr */
                        atomic_store_explicit(&target_entry->ptr, ptr,
                                              memory_order_release);
                        
                        /* Mark old slot as TOMBSTONE */
                        atomic_store_explicit(&entry->ptr, HEAP_ENTRY_TOMBSTONE,
                                              memory_order_release);
                        compacted++;
                        break;
                    }
                }
            }
        }
    }
    
    atomic_fetch_add(&g_memprof.compaction_moves, compacted);
    return (int)compacted;
}

/**
 * Monitoring: Track average probe length to trigger compaction
 */
static inline void update_probe_stats(int probe_length) {
    /* Exponential moving average: new_avg = 0.99 * old_avg + 0.01 * sample */
    /* For simplicity, just track max and trigger when concerning */
    uint64_t prev_max = atomic_load(&g_memprof.max_probe_length);
    if (probe_length > prev_max) {
        atomic_store(&g_memprof.max_probe_length, probe_length);
        if (probe_length > 32) {
            /* Log warning: compaction may be needed */
            atomic_fetch_add(&g_memprof.probe_length_warnings, 1);
        }
    }
}
```

**Compaction Trigger Policy:**

| Metric | Threshold | Action |
|--------|-----------|--------|
| `max_probe_length` | < 16 | Normal operation |
| `max_probe_length` | 16-32 | Log warning to stats |
| `max_probe_length` | > 32 | Trigger background compaction |
| `tombstones / capacity` | > 50% | Force compaction on next sample |

**v1.0 Stance**: Compaction is **optional** and **not implemented** in v1.0. The tombstone
reuse mechanism provides adequate performance for most workloads. Compaction will be added
in v1.1 if field experience shows degradation in long-running processes.

### 12.4 Memory Safety

**Heap map overflow**: When load factor exceeds 80%, new samples are dropped (not crashed).

**Stack table overflow**: When table is full, samples get stack_id = MAX_STACK_ID (graceful degradation).

**Invalid frame pointers**: Stack walker validates pointers before dereferencing.

**Packed metadata atomicity**: All metadata (stack_id, size, weight) is stored in a single 64-bit atomic field, preventing torn reads during concurrent snapshot.

### 12.5 Free-Threading (Py_GIL_DISABLED)

The memory profiler is **safe** for free-threaded Python because:

1. All synchronization is via atomics (no GIL dependency)
2. No Python API calls in hot path
3. Symbol resolution happens in safe context with appropriate locking

### 12.6 Fork Safety (multiprocessing)

**Problem**: Python's `multiprocessing` module (default on Linux) uses `fork()`. If a thread
is mid-operation during fork, the child process inherits corrupted state.

**Risks:**

| Scenario | Impact | Severity |
|----------|--------|----------|
| Thread in `heap_map_reserve()` | RESERVED slot orphaned forever | Low |
| Thread in `bloom_rebuild()` | Child may have inconsistent filter | Medium |
| Thread holding `bloom_rebuild_in_progress` | Rebuild permanently blocked | Medium |

**Mitigation:**

```c
/* Register fork handlers via pthread_atfork() */
static void memprof_prefork(void) {
    /* Acquire any "soft locks" (atomic flags used as locks) */
    while (atomic_exchange(&g_memprof.bloom_rebuild_in_progress, 1)) {
        /* Spin until we own it - brief, fork is rare */
        sched_yield();
    }
}

static void memprof_postfork_parent(void) {
    /* Release lock in parent */
    atomic_store(&g_memprof.bloom_rebuild_in_progress, 0);
}

static void memprof_postfork_child(void) {
    /* In child: Reset all state, profiler is effectively disabled
     * until explicitly restarted. Leaked RESERVED slots are acceptable. */
    atomic_store(&g_memprof.bloom_rebuild_in_progress, 0);
    atomic_store(&g_memprof.active_alloc, 0);
    atomic_store(&g_memprof.active_free, 0);
    /* TLS is per-thread, child's main thread gets fresh TLS */
}

int memprof_register_fork_handlers(void) {
    return pthread_atfork(memprof_prefork, 
                          memprof_postfork_parent,
                          memprof_postfork_child);
}
```

**Verdict**: Fork safety is "best effort". A few orphaned heap map entries in child
processes are acceptable. For production multiprocessing, recommend `spawn` start method:

```python
import multiprocessing
multiprocessing.set_start_method('spawn')  # Avoids fork issues entirely
```

**vfork() - The Dangerous Cousin:**

`vfork()` is more dangerous than `fork()` because the child **shares** the parent's
address space until `exec()`. The parent is suspended, but any writes in the child
(including our lock-free data structures) corrupt the parent's state.

| Operation | fork() | vfork() |
|-----------|--------|---------|
| Address space | Copied (COW) | Shared |
| Parent state | Preserved | **Corrupted if child writes** |
| Safe for profiler | Yes (with handlers) | **No** |

**Risk**: `subprocess.Popen` on some platforms may use `vfork()` under the hood
(especially with `close_fds=True` or on older systems).

**Mitigation:**

```c
/* Detect vfork and disable profiler in child immediately.
 * 
 * Detection is platform-specific and imperfect:
 * - Linux: Check /proc/self/stat for 'Z' (zombie) parent
 * - Alternative: Set a flag in malloc hook, check in exec wrapper
 *
 * PRACTICAL APPROACH: Since vfork children typically call exec() immediately,
 * the window for corruption is small. We disable sampling on the first
 * allocation after fork/vfork by checking PID change.
 */
static pid_t g_init_pid = 0;

static inline int in_forked_child(void) {
    if (UNLIKELY(g_init_pid == 0)) {
        g_init_pid = getpid();
        return 0;
    }
    return getpid() != g_init_pid;
}

void* malloc(size_t size) {
    /* Quick check: if PID changed, we're in a fork/vfork child */
    if (UNLIKELY(in_forked_child())) {
        /* Disable profiler until explicitly restarted */
        atomic_store(&g_memprof.active_alloc, 0);
        return real_malloc(size);  /* Bypass profiler entirely */
    }
    /* ... normal path ... */
}
```

**Best Practice**: For applications using `subprocess`, prefer `spawn` or ensure
`exec()` is called immediately after fork. The profiler auto-disables in children.

⚠️ **UNDEFINED BEHAVIOR WARNING**:

The PID-check mitigation only detects vfork **after the first allocation** in the child.
If the vforked child writes to global state (e.g., `g_memprof` flags, TLS if shared)
**before** any allocation, it will corrupt the parent's profiler state.

**Known safe**: Python's `subprocess.Popen` (uses fork+exec, not raw vfork)

**Known unsafe**: C extensions calling raw `vfork()` that mutate memory before `exec()`

**Official stance**: spprof behavior is **undefined** in the presence of raw `vfork()`
calls that write to memory before calling `exec()`. This is consistent with POSIX,
which restricts vfork children to only calling `_exit()` or `exec*()` functions.

### 12.7 Resolver Timing and Lock Contention

**Critical Design Decision: When does resolution happen?**

⚠️ **dl_iterate_phdr Lock Hazard**:

Symbol resolution via `dladdr()` internally calls `dl_iterate_phdr()`, which holds
a global lock in the dynamic linker. If your application uses `dlopen()`/`dlclose()`
concurrently (plugin systems, JIT compilers, Python extension loading), a background
resolver thread can cause:

1. **Lock contention**: Resolver blocks dlopen, slowing module loads
2. **Priority inversion**: Low-priority resolver thread holds lock, blocking main thread
3. **Deadlock risk**: Complex dl* call sequences can deadlock

**Recommended Resolution Strategy:**

| Timing | Use Case | Lock Risk |
|--------|----------|-----------|
| **On-stop** (default) | Production profiling | ✅ None (single-threaded) |
| **On-snapshot** | Periodic monitoring | ✅ None (caller's thread) |
| **Background** | Real-time visualization | ⚠️ Lock contention possible |

**Default behavior**: Resolution happens synchronously when `stop()` or `get_snapshot()`
is called, NOT in a background thread. This avoids dl* lock contention entirely.

**Optional background resolution** (compile-time flag, not default):
- Use only for real-time dashboards
- Requires careful testing with your dlopen patterns
- Consider rate-limiting resolution to reduce lock pressure

### 12.8 Resolver Graceful Degradation

**Scenario**: JIT-compiled code (PyTorch, Numba, JAX) can generate thousands of unique native call stacks rapidly. Even synchronous resolution may be slow.

**Behavior**: The system degrades gracefully:

1. **Stack capture continues unimpeded** - Raw PCs are stored immediately
2. **Resolution is lazy** - Stacks are marked "unresolved" until explicitly resolved
3. **Snapshots can show raw addresses** - If caller requests immediate snapshot, frames show `0x7fff12345678` instead of function names
4. **No data loss** - All sampled allocations are tracked; only symbol names are deferred

**Mitigation for extreme cases**:

```c
/* Resolver can expose backpressure metrics */
typedef struct {
    uint64_t stacks_pending;      /* Stacks awaiting resolution */
    uint64_t resolve_rate;        /* Stacks resolved per second */
    uint64_t estimated_lag_ms;    /* Current backlog / rate */
} ResolverStats;
```

**Recommendation**: For JIT-heavy workloads, call `get_snapshot(resolve=True)` which
blocks until all stacks are resolved, or `get_snapshot(resolve=False)` for immediate
return with raw addresses (resolve later via `snapshot.resolve()`).

---

## 13. Implementation Plan

### Phase 1: Core Infrastructure (Week 1-2)

- [ ] **T1.1**: Heap map with two-phase insert (reserve/finalize pattern)
- [ ] **T1.2**: **Bloom filter for free() hot path** (critical for performance)
- [ ] **T1.3**: Stack intern table (256K entries, fixed capacity)
- [ ] **T1.4**: Sampling engine (sampling.c, TLS management, PRNG)
- [ ] **T1.5**: Stack capture (frame pointer walking, reuse framewalker.c)
- [ ] **T1.6**: Unit tests for data structures (concurrent stress tests)

### Phase 2: Platform Interposition (Week 3-4)

- [ ] **T2.1**: macOS malloc_logger integration (lowest risk, good test platform)
- [ ] **T2.2**: Linux LD_PRELOAD library (most complex, higher risk)
- [ ] **T2.3**: Windows Detours integration (lowest priority)
- [ ] **T2.4**: Platform-specific tests

### Phase 3: Python Bindings (Week 5)

- [ ] **T3.1**: C extension module (python_bindings.c)
- [ ] **T3.2**: Python wrapper (memprof.py)
- [ ] **T3.3**: Integration tests
- [ ] **T3.4**: Python GC epoch tracking (optional audit hook integration)

### Phase 4: Symbol Resolution (Week 6)

- [ ] **T4.1**: Native symbol resolution (dladdr, DbgHelp)
- [ ] **T4.2**: Mixed-mode stack merging (reuse resolver.c "Trim & Sandwich")
- [ ] **T4.3**: Speedscope output format adaptation

### Phase 5: Polish and Documentation (Week 7-8)

- [ ] **T5.1**: Performance benchmarks (overhead validation)
- [ ] **T5.2**: Documentation
- [ ] **T5.3**: Example scripts
- [ ] **T5.4**: CI integration

### Milestones

| Milestone | Target | Deliverable |
|-----------|--------|-------------|
| M1: Alpha | Week 4 | Working profiler on Linux |
| M2: Beta | Week 6 | All platforms, Python API |
| M3: Release | Week 8 | Documentation, benchmarks |

---

## 14. Testing Strategy

### 14.1 Unit Tests

```python
# tests/test_memprof_heap_map.py

def test_heap_map_insert_and_remove():
    """Test basic insert/remove operations."""
    heap_map_init()
    
    ptr = 0x7FFF12345678
    assert heap_map_insert(ptr, stack_id=1, size=1024, weight=512, ts=0) == 1
    
    out_stack_id, out_size, out_weight, out_duration = (0, 0, 0, 0)
    assert heap_map_remove(ptr, &out_stack_id, &out_size, &out_weight, &out_duration) == 1
    assert out_stack_id == 1
    assert out_size == 1024
    
    heap_map_destroy()

def test_heap_map_concurrent_insert():
    """Test thread-safe concurrent insertions."""
    ...

def test_stack_intern_deduplication():
    """Test that identical stacks get same ID."""
    ...
```

### 14.2 Integration Tests

```python
# tests/test_memprof_integration.py

def test_basic_profiling():
    """Test basic start/stop/snapshot cycle."""
    import spprof.memprof as memprof
    
    memprof.start(sampling_rate_kb=64)
    
    # Allocate known amount
    data = bytearray(10 * 1024 * 1024)  # 10 MB
    
    snapshot = memprof.get_snapshot()
    memprof.stop()
    
    # Should have captured some samples
    assert snapshot.live_samples > 0
    # Estimated heap should be in reasonable range
    assert 5_000_000 < snapshot.estimated_heap_bytes < 50_000_000

def test_numpy_allocation():
    """Test capturing NumPy allocations."""
    import numpy as np
    import spprof.memprof as memprof
    
    memprof.start(sampling_rate_kb=128)
    arr = np.zeros((1000, 1000), dtype=np.float64)  # ~8 MB
    snapshot = memprof.get_snapshot()
    memprof.stop()
    
    # Should see allocation site in stack
    top = snapshot.top_allocators(1)[0]
    assert 'numpy' in top['function'].lower() or 'zeros' in str(top['stack'])
```

### 14.3 Stress Tests

```python
# tests/test_memprof_stress.py

def test_high_allocation_rate():
    """Test with 1M+ allocations/sec."""
    import spprof.memprof as memprof
    
    memprof.start(sampling_rate_kb=512)
    
    for _ in range(1_000_000):
        x = [0] * 100  # Small allocation
        del x
    
    stats = memprof.get_stats()
    memprof.stop()
    
    # Should have reasonable number of samples
    assert 100 < stats.total_samples < 10000

def test_concurrent_allocation():
    """Test with multiple threads allocating."""
    import threading
    import spprof.memprof as memprof
    
    memprof.start(sampling_rate_kb=256)
    
    def allocator():
        for _ in range(10000):
            x = bytearray(1000)
    
    threads = [threading.Thread(target=allocator) for _ in range(10)]
    for t in threads: t.start()
    for t in threads: t.join()
    
    snapshot = memprof.get_snapshot()
    memprof.stop()
    
    assert snapshot.total_samples > 0
```

### 14.4 Memory Safety Tests

```python
# tests/test_memprof_safety.py

def test_no_crash_on_reentrant_allocation():
    """Test that re-entrant allocations don't crash."""
    # This is implicitly tested by any test that uses the profiler,
    # since dladdr() and other internals allocate.
    ...

def test_graceful_degradation_on_full_heap_map():
    """Test that full heap map drops samples, doesn't crash."""
    ...

def test_no_memory_leak():
    """Test that profiler doesn't leak memory."""
    import tracemalloc
    
    tracemalloc.start()
    
    for _ in range(10):
        memprof.start(sampling_rate_kb=512)
        data = bytearray(1024 * 1024)
        memprof.stop()
        memprof.shutdown()
    
    current, peak = tracemalloc.get_traced_memory()
    tracemalloc.stop()
    
    # Should not accumulate memory across runs
    assert current < 10_000_000  # Less than 10 MB
```

---

## 15. Future Enhancements

### 15.1 Short Term (Post-1.0)

1. **Temporal analysis**: Track allocation churn rate over time
2. **Custom allocator support**: Hook jemalloc, tcmalloc, mimalloc
3. **Differential snapshots**: Compare two snapshots to find leaks
4. **Configurable stack table size**: Runtime-adjustable for large codebases
5. **SIMD stack hashing**: AVX2/NEON accelerated FNV-1a for stack interning

### 15.2 Medium Term

1. **Live visualization**: Real-time heap graph in web UI
2. **Allocation flamegraphs**: Integrate with existing flamegraph output
3. **Leak detection heuristics**: Flag likely leaks based on lifetime
4. **RSS correlation**: Correlate sampled heap with actual RSS

### 15.3 Long Term / Research

1. **Hardware sampling**: Use perf_mem_record for lower overhead
2. **eBPF integration**: Kernel-level allocation tracing
3. **ML-based anomaly detection**: Identify unusual allocation patterns
4. **Distributed profiling**: Aggregate profiles across cluster

---

## Appendix A: Glossary

| Term | Definition |
|------|------------|
| **Poisson sampling** | Statistical sampling where inter-arrival times follow exponential distribution |
| **Interposition** | Technique to intercept function calls by replacing function pointers |
| **GOT** | Global Offset Table - contains resolved addresses of external functions |
| **PLT** | Procedure Linkage Table - lazy binding trampoline for dynamic linking |
| **TLS** | Thread-Local Storage - per-thread memory area |
| **CAS** | Compare-And-Swap - atomic read-modify-write operation |
| **Tombstone** | Marker value indicating deleted entry in hash table |
| **Stack interning** | Deduplicating call stacks by hashing and storing unique copies |

---

## Appendix B: References

1. **Google tcmalloc heap profiler**: https://gperftools.github.io/gperftools/heapprofile.html
2. **jemalloc profiling**: https://jemalloc.net/jemalloc.3.html
3. **Poisson sampling theory**: "Sampling Techniques" by Cochran
4. **Lock-free programming**: "C++ Concurrency in Action" by Williams
5. **malloc_logger**: Apple Developer Documentation (undocumented but stable)
6. **Microsoft Detours**: https://github.com/microsoft/Detours

---

## Appendix C: Revision History

| Version | Date | Changes |
|---------|------|---------|
| 1.0.0-draft | Dec 2024 | Initial specification |
| 1.0.1-draft | Dec 2024 | Technical refinements based on expert review: |
| | | • **Heap map**: Packed metadata (24B/entry) for atomic consistency |
| | | • **Bloom filter**: Moved to Phase 1 (critical for free() performance) |
| | | • **Two-phase insert**: Reserve→finalize pattern prevents free-before-insert race |
| | | • **macOS malloc_logger**: Thread-safe installation with atomic flag |
| | | • **Exponential variate**: Improved clamping (1e-10 for ~23× max threshold) |
| | | • **GC epoch tracking**: Optional Python audit hook integration |
| | | • **Implementation plan**: Reordered for risk mitigation |
| 1.0.2-draft | Dec 2024 | Hardening based on second expert review: |
| | | • **ABA problem**: Clarified real race is address reuse, not malloc timing |
| | | • **Wait-free free()**: Replaced spinning with "death during birth" pattern |
| | | • **Bloom saturation**: Added rebuild strategy for long-running processes |
| | | • **Frame pointer docs**: Explicit requirements and DWARF fallback option |
| | | • **Pre-hook vs post-hook**: Documented platform timing requirements |
| | | • **State machine**: Full state transition diagram for heap map entries |
| | | • **Global state**: Added bloom rebuild fields and death_during_birth counter |
| 1.0.3-draft | Dec 2024 | Third expert review - production hardening: |
| | | • **macOS Zombie Killer**: Timestamp heuristic guards completed allocations |
| | | • **Bloom filter fix**: Unified to pointer-only (mmap) for atomic swap |
| | | • **Memory footprint**: Dynamic stack table (2MB initial vs 140MB fixed) |
| | | • **Frame pointer UX**: Runtime detection with one-time stderr warning |
| | | • **Fork safety**: Added pthread_atfork handlers for multiprocessing |
| | | • **RNG seeding**: Mixed in getpid() + /dev/urandom for parallel workers |
| | | • **Signal safety**: Replaced usleep with nanosleep (POSIX.1-2001) |
| 1.0.4-draft | Dec 2024 | Fourth expert review - crash-proofing: |
| | | • **Bloom filter safety**: Intentionally leak old filters (no munmap) |
| | | • **macOS sequence counter**: Deterministic zombie detection, not heuristic |
| | | • **dlsym recursion trap**: Bootstrap heap for init-time allocations |
| | | • **vfork handling**: Auto-disable in forked children via PID check |
| | | • **Frame Pointer Health**: Added to HeapSnapshot for quality assessment |
| | | • **Variance warning**: Documented small allocation bias in short runs |
| | | • **HeapMapEntry update**: Added birth_seq field for sequence-based ordering |
| 1.0.5-draft | Dec 2024 | Fifth expert review - final polish (APPROVED): |
| | | • **vfork undefined behavior**: Explicit warning for raw vfork() edge cases |
| | | • **dlsym failure handling**: Permanent bootstrap mode if dlsym returns NULL |
| | | • **Bootstrap heap**: Increased from 8KB to 64KB for complex init paths |
| | | • **Virtual Memory vs RSS**: Clarified what we measure vs physical memory |
| | | • **Shutdown safety**: Document as one-way door, never munmap while running |
| | | • **Stack table resize**: Added mremap portability notes (Linux vs macOS/Win) |
| | | • **Error bounds**: Added count-based vs byte-weighted metric variance note |
| 1.0.6 | Dec 2024 | Sixth expert review - FINAL (APPROVED): |
| | | • **Fail-fast over fail-late**: dlsym failure now aborts immediately |
| | | • **Tombstone reuse**: heap_map_reserve recycles TOMBSTONE slots |
| | | • **DWARF warning**: Strong performance warning (100-1000× slower) |
| | | • **LSan compliance**: Leaked Bloom filters cleaned up at shutdown |
| | | • **Resolver timing**: Clarified on-stop default to avoid dl* lock contention |
| | | • **State machine update**: TOMBSTONE → RESERVED transition added |
| 1.0.7 | Dec 2024 | Seventh expert review - comprehensive hardening: |
|| | | • **Tombstone compaction**: Added optional Robin Hood-style compaction strategy with monitoring |
|| | | • **Platform address validation**: Platform-specific ADDR_MAX_USER macros (x86_64/ARM64/i386) |
|| | | • **Size clamping docs**: Explicit warning about 16MB limit impact on per-allocation display |
|| | | • **dlsym error handling**: Use compile-time sizeof()-1 instead of strlen() for safety |
|| | | • **Lifecycle state machine**: Full state diagram with allowed operations per state |
|| | | • **is_python_interpreter_frame()**: Complete implementation with known limitations |
|| | | • **Windows EXPERIMENTAL**: Marked with limitations table, HeapAlloc/VirtualAlloc not tracked |
|| | | • **usleep→nanosleep**: Fixed signal safety in memprof_darwin_remove() |
|| | | • **ASLR documentation**: Stack IDs not comparable across process restarts |
|| | | • **mmap bypass docs**: Direct mmap() calls not tracked, glibc threshold note |
|| | | • **Profiler overhead**: Individual sampled allocations show inflated latency |
|| | | • **Bloom rebuild clarity**: Non-atomic access safe before publication (with comment) |

---

*End of specification*