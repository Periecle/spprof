/* SPDX-License-Identifier: MIT
 * memprof.h - spprof Memory Allocation Profiler
 *
 * Core types, constants, and global state for the memory profiler.
 * This header is the main entry point for the memprof subsystem.
 *
 * ARCHITECTURE:
 *   The memory profiler uses Poisson sampling to capture allocation stacks
 *   with controlled overhead. Key components:
 *
 *   - Sampling Engine (sampling.h/c): Per-thread TLS state with exponential
 *     inter-sample intervals. Hot path is ~5 cycles.
 *
 *   - Heap Map (heap_map.h/c): Lock-free hash table tracking sampled
 *     allocations. Uses two-phase insert for race safety.
 *
 *   - Stack Intern (stack_intern.h/c): Deduplicates call stacks into 32-bit
 *     IDs for compact storage.
 *
 *   - Bloom Filter (bloom.h/c): Optimizes free() path - 99.99% of frees
 *     are non-sampled and skip the heap map lookup.
 *
 * THREAD SAFETY:
 *   All data structures use lock-free algorithms with atomic operations.
 *   No mutexes are used in hot paths.
 *
 * PLATFORM SUPPORT:
 *   - Linux: glibc malloc hooks or LD_PRELOAD interposition
 *   - macOS: malloc_zone logging hooks
 *   - Windows: Heap API hooks (experimental)
 *
 * Copyright (c) 2024 spprof contributors
 */

#ifndef SPPROF_MEMPROF_H
#define SPPROF_MEMPROF_H

/* _GNU_SOURCE for Linux-specific features (mremap, pthread_atfork, dladdr) */
#if defined(__linux__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>

/* ============================================================================
 * Configuration Constants
 * ============================================================================ */

/* Maximum native stack depth to capture */
#define MEMPROF_MAX_STACK_DEPTH 64

/* Live heap map capacity (must be power of 2) */
#define MEMPROF_HEAP_MAP_CAPACITY (1 << 20)  /* 1M entries, ~24MB */
#define MEMPROF_HEAP_MAP_MASK (MEMPROF_HEAP_MAP_CAPACITY - 1)

/* Stack intern table - dynamic sizing
 * 
 * DESIGN NOTE: Larger initial capacity reduces resize frequency.
 * Each StackEntry is ~544 bytes, so:
 *   - 16K entries = ~8.5MB
 *   - 64K entries = ~35MB
 *   - 128K entries = ~70MB
 *
 * Production apps can easily hit 64K unique stacks, so we default
 * to 64K initial to avoid resize during profiling (resize is NOT
 * fully thread-safe without RCU).
 */
#define MEMPROF_STACK_TABLE_INITIAL  (1 << 16)   /* 64K entries (~35MB) */
#define MEMPROF_STACK_TABLE_MAX_DEFAULT (1 << 18) /* 256K entries (~140MB) */
#define MEMPROF_STACK_TABLE_GROW_THRESHOLD 75    /* Grow at 75% load */

/* Probe limit for open-addressing */
#define MEMPROF_MAX_PROBE 128

/* Default sampling rate (bytes between samples) */
#define MEMPROF_DEFAULT_SAMPLING_RATE (512 * 1024)  /* 512 KB */

/* Bloom filter parameters */
#define BLOOM_SIZE_BITS (1 << 20)       /* 1M bits */
#define BLOOM_SIZE_BYTES (BLOOM_SIZE_BITS / 8)  /* 128KB */
#define BLOOM_HASH_COUNT 4

/* ============================================================================
 * HeapMapEntry Field Limits
 * ============================================================================ */

/*
 * DESIGN NOTE (2024): We previously packed stack_id, size, and weight into
 * a single 64-bit metadata field. This caused the "16MB Lie" problem where
 * large allocations (common in ML workloads) were misreported.
 *
 * New design: Store fields separately with full precision.
 * - stack_id: 32 bits (matches stack table index type)
 * - size: 64 bits (no limit - can track any allocation)
 * - weight: 32 bits (supports sampling rates up to 4GB)
 *
 * HeapMapEntry is now 48 bytes (was 32), trading ~50% more memory for
 * accurate profiling of large allocations.
 */

/* Maximum stack_id is bounded by stack table capacity */
#define MAX_STACK_ID   UINT32_MAX

/* No artificial limit on allocation size */
#define MAX_ALLOC_SIZE UINT64_MAX

/* Weight limit - 32 bits supports sampling rates up to 4GB */
#define MAX_WEIGHT     UINT32_MAX

/* ============================================================================
 * Heap Map Entry State Machine
 * ============================================================================ */

#define HEAP_ENTRY_EMPTY     ((uintptr_t)0)
#define HEAP_ENTRY_RESERVED  ((uintptr_t)1)  /* Insert in progress */
#define HEAP_ENTRY_TOMBSTONE (~(uintptr_t)0)

/* ============================================================================
 * Forward Declarations
 * ============================================================================ */

struct HeapMapEntry;
struct StackEntry;
struct MemProfThreadState;
struct MemProfGlobalState;
struct MixedStackCapture;

/* ============================================================================
 * HeapMapEntry - Single entry in the live heap map (48 bytes)
 * ============================================================================ */

typedef struct HeapMapEntry {
    _Atomic uintptr_t ptr;        /* Key: allocated pointer (state encoded) */
    _Atomic uint32_t  stack_id;   /* Interned stack trace ID */
    _Atomic uint32_t  weight;     /* Sampling weight (= sampling_rate) */
    _Atomic uint64_t  size;       /* Allocation size in bytes (full 64-bit) */
    _Atomic uint64_t  birth_seq;  /* Sequence number at allocation time */
    uint64_t          timestamp;  /* Wall clock time (nanoseconds) */
} HeapMapEntry;

/* ============================================================================
 * StackEntry - Interned call stack (~544 bytes)
 * ============================================================================ */

#define STACK_FLAG_RESOLVED        0x0001
#define STACK_FLAG_PYTHON_ATTR     0x0002
#define STACK_FLAG_TRUNCATED       0x0004

/* Stack hash state markers:
 *   0 = empty slot (available)
 *   1 = reserved (being written by a thread, do not read data yet)
 *   >= 2 = valid hash (data is fully written and readable)
 */
#define STACK_HASH_EMPTY           0ULL
#define STACK_HASH_RESERVED        1ULL

typedef struct StackEntry {
    _Atomic uint64_t hash;        /* FNV-1a hash for lookup; 0=empty, 1=reserved, >=2=valid */
    uint16_t depth;               /* Number of valid native frames */
    uint16_t flags;               /* RESOLVED, PYTHON_ATTRIBUTED, etc. */
    uintptr_t frames[MEMPROF_MAX_STACK_DEPTH];  /* Raw return addresses */
    
    /* Python frames (code object pointers from framewalker) */
    uintptr_t python_frames[MEMPROF_MAX_STACK_DEPTH];
    uint16_t python_depth;
    
    /* Resolved symbols (lazily populated by async resolver) */
    char** function_names;        /* Array of function name strings */
    char** file_names;            /* Array of file name strings */
    int*   line_numbers;          /* Array of line numbers */
} StackEntry;

/* ============================================================================
 * MemProfThreadState - Per-thread sampling state (TLS, ~1 KB)
 * ============================================================================ */

typedef struct MemProfThreadState {
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

/* ============================================================================
 * MemProfGlobalState - Singleton profiler state
 * ============================================================================ */

typedef struct MemProfGlobalState {
    /* Configuration (immutable after init) */
    uint64_t sampling_rate;       /* Average bytes between samples */
    int      capture_python;      /* Also hook PyMem allocator */
    int      resolve_on_stop;     /* Resolve symbols when profiling stops */
    
    /* State (atomic) - Separate flags for alloc/free tracking */
    _Atomic int active_alloc;     /* Track new allocations (start→stop) */
    _Atomic int active_free;      /* Track frees (start→shutdown) */
    _Atomic int initialized;      /* Init completed */
    _Atomic int shutdown;         /* One-way shutdown flag */
    
    /* Data structures (allocated once via mmap) */
    HeapMapEntry* heap_map;       /* Live allocations */
    StackEntry*   stack_table;    /* Interned stacks */
    _Atomic uint32_t stack_count; /* Number of unique stacks */
    size_t stack_table_capacity;  /* Current stack table capacity */
    
    /* Bloom filter (swappable for rebuild)
     * 
     * DOUBLE-INSERT STRATEGY: During rebuild, bloom_staging_filter_ptr points
     * to the new filter being built. bloom_add() writes to BOTH active and
     * staging filters to prevent race conditions. */
    _Atomic(_Atomic uint8_t*) bloom_filter_ptr;         /* Current active filter */
    _Atomic(_Atomic uint8_t*) bloom_staging_filter_ptr; /* New filter during rebuild (NULL when not rebuilding) */
    _Atomic uint64_t bloom_ones_count;                  /* Approximate bits set */
    _Atomic int bloom_rebuild_in_progress;              /* Rebuild lock */
    
    /* Global sequence counter for ABA detection */
    _Atomic uint64_t global_seq;
    
    /* Global statistics (atomic) */
    _Atomic uint64_t total_samples;
    _Atomic uint64_t total_frees_tracked;
    _Atomic uint64_t heap_map_collisions;
    _Atomic uint64_t heap_map_insertions;
    _Atomic uint64_t heap_map_deletions;
    _Atomic uint64_t heap_map_full_drops;
    _Atomic uint64_t stack_table_collisions;
    _Atomic uint64_t stack_table_saturations;  /* Times stack table was full */
    _Atomic uint64_t bloom_rebuilds;
    _Atomic uint64_t death_during_birth;
    _Atomic uint64_t zombie_races_detected;
    _Atomic uint64_t tombstones_recycled;
    _Atomic uint64_t shallow_stack_warnings;
    
    /* Platform-specific state */
    void* platform_state;
} MemProfGlobalState;

/* Global instance */
extern MemProfGlobalState g_memprof;

/* ============================================================================
 * MixedStackCapture - Combined Python + Native frames
 * ============================================================================ */

typedef struct MixedStackCapture {
    uintptr_t native_pcs[MEMPROF_MAX_STACK_DEPTH];
    int native_depth;
    uintptr_t python_code_ptrs[MEMPROF_MAX_STACK_DEPTH];
    int python_depth;
} MixedStackCapture;

/* ============================================================================
 * Statistics Structure (for Python API)
 * ============================================================================ */

typedef struct MemProfStats {
    uint64_t total_samples;
    uint64_t live_samples;
    uint64_t freed_samples;
    uint32_t unique_stacks;
    uint64_t estimated_heap_bytes;
    float    heap_map_load_percent;
    uint64_t collisions;
    uint64_t sampling_rate_bytes;
    uint64_t shallow_stack_warnings;
    uint64_t death_during_birth;
    uint64_t zombie_races_detected;
} MemProfStats;

/* ============================================================================
 * Core Lifecycle API
 * ============================================================================ */

/**
 * Initialize the memory profiler.
 *
 * @param sampling_rate  Average bytes between samples
 * @return 0 on success, -1 on error
 */
int memprof_init(uint64_t sampling_rate);

/**
 * Start memory profiling.
 * @return 0 on success, -1 if already running or not initialized
 */
int memprof_start(void);

/**
 * Stop memory profiling (new allocations only, frees still tracked).
 * @return 0 on success, -1 if not running
 */
int memprof_stop(void);

/**
 * Get snapshot of live allocations.
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
 * @return Number of stacks resolved
 */
int memprof_resolve_symbols(void);

/**
 * Shutdown profiler (one-way door).
 */
void memprof_shutdown(void);

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

/**
 * Get monotonic time in nanoseconds.
 */
uint64_t memprof_get_monotonic_ns(void);

/**
 * Branch prediction hints
 */
#ifdef __GNUC__
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define LIKELY(x)   (x)
#define UNLIKELY(x) (x)
#endif

#endif /* SPPROF_MEMPROF_H */

