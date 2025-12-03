/* SPDX-License-Identifier: MIT
 * memprof.h - spprof Memory Allocation Profiler
 *
 * Core types, constants, and global state for the memory profiler.
 * This header is the main entry point for the memprof subsystem.
 */

#ifndef SPPROF_MEMPROF_H
#define SPPROF_MEMPROF_H

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

/* Stack intern table - dynamic sizing */
#define MEMPROF_STACK_TABLE_INITIAL  (1 << 12)   /* 4K entries (~2MB) */
#define MEMPROF_STACK_TABLE_MAX_DEFAULT (1 << 16) /* 64K entries (~35MB) */
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
 * Packed Metadata Macros (24 bytes per HeapMapEntry)
 * ============================================================================ */

/* Format: stack_id (20 bits) | size (24 bits) | weight (20 bits) = 64 bits */
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
 * HeapMapEntry - Single entry in the live heap map (24 bytes)
 * ============================================================================ */

typedef struct HeapMapEntry {
    _Atomic uintptr_t ptr;        /* Key: allocated pointer (state encoded) */
    _Atomic uint64_t  metadata;   /* Packed: stack_id | size | weight */
    _Atomic uint64_t  birth_seq;  /* Sequence number at allocation time */
    uint64_t          timestamp;  /* Wall clock time (nanoseconds) */
} HeapMapEntry;

/* ============================================================================
 * StackEntry - Interned call stack (~544 bytes)
 * ============================================================================ */

#define STACK_FLAG_RESOLVED        0x0001
#define STACK_FLAG_PYTHON_ATTR     0x0002
#define STACK_FLAG_TRUNCATED       0x0004

typedef struct StackEntry {
    _Atomic uint64_t hash;        /* FNV-1a hash for lookup; 0 = empty */
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
    
    /* Bloom filter (swappable for rebuild) */
    _Atomic(_Atomic uint8_t*) bloom_filter_ptr;  /* Current active filter */
    _Atomic uint64_t bloom_ones_count;           /* Approximate bits set */
    _Atomic int bloom_rebuild_in_progress;       /* Rebuild lock */
    
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

