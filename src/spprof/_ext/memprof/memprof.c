/* SPDX-License-Identifier: MIT
 * memprof.c - Memory profiler core lifecycle management
 *
 * This file orchestrates initialization, start/stop, snapshot, and shutdown
 * of the memory profiler subsystem.
 */

#include "memprof.h"
#include "heap_map.h"
#include "stack_intern.h"
#include "bloom.h"
#include "sampling.h"
#include "stack_capture.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

/* ============================================================================
 * Global State Definition
 * ============================================================================ */

MemProfGlobalState g_memprof = {0};

/* ============================================================================
 * Platform-Specific Hooks (forward declarations)
 * ============================================================================ */

#if defined(__APPLE__)
extern int memprof_darwin_install(void);
extern void memprof_darwin_remove(void);
#elif defined(__linux__)
extern int memprof_linux_install(void);
extern void memprof_linux_remove(void);
#elif defined(_WIN32)
extern int memprof_windows_install(void);
extern void memprof_windows_remove(void);
#endif

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

/* Cached Windows frequency (queried once) */
#ifdef _WIN32
static LARGE_INTEGER g_qpc_frequency = {0};
static volatile LONG g_qpc_init = 0;
#endif

uint64_t memprof_get_monotonic_ns(void) {
#ifdef _WIN32
    /* Cache QPC frequency - it's constant for system lifetime */
    if (InterlockedCompareExchange(&g_qpc_init, 1, 0) == 0) {
        QueryPerformanceFrequency(&g_qpc_frequency);
    }
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    /* Use 128-bit math to avoid overflow: (counter * 1e9) / freq */
    return (uint64_t)(((__int128)counter.QuadPart * 1000000000LL) / 
                      g_qpc_frequency.QuadPart);
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;  /* Fallback on error */
    }
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

/* ============================================================================
 * Initialization
 * ============================================================================ */

int memprof_init(uint64_t sampling_rate) {
    /* Check if already initialized - use acquire to sync with previous release */
    if (atomic_load_explicit(&g_memprof.initialized, memory_order_acquire)) {
        return 0;  /* Idempotent */
    }
    
    /* Check if we've been shutdown (cannot reinitialize after shutdown) */
    if (atomic_load_explicit(&g_memprof.shutdown, memory_order_acquire)) {
        return -1;
    }
    
    /* Set configuration */
    g_memprof.sampling_rate = (sampling_rate > 0) ? 
                               sampling_rate : MEMPROF_DEFAULT_SAMPLING_RATE;
    g_memprof.capture_python = 1;
    g_memprof.resolve_on_stop = 1;
    
    /* Initialize atomic counters BEFORE data structures
     * to ensure consistent state if init is interrupted */
    atomic_store_explicit(&g_memprof.active_alloc, 0, memory_order_relaxed);
    atomic_store_explicit(&g_memprof.active_free, 0, memory_order_relaxed);
    atomic_store_explicit(&g_memprof.global_seq, 0, memory_order_relaxed);
    atomic_store_explicit(&g_memprof.total_samples, 0, memory_order_relaxed);
    atomic_store_explicit(&g_memprof.total_frees_tracked, 0, memory_order_relaxed);
    atomic_store_explicit(&g_memprof.heap_map_collisions, 0, memory_order_relaxed);
    atomic_store_explicit(&g_memprof.heap_map_insertions, 0, memory_order_relaxed);
    atomic_store_explicit(&g_memprof.heap_map_deletions, 0, memory_order_relaxed);
    atomic_store_explicit(&g_memprof.heap_map_full_drops, 0, memory_order_relaxed);
    atomic_store_explicit(&g_memprof.stack_table_collisions, 0, memory_order_relaxed);
    atomic_store_explicit(&g_memprof.bloom_rebuilds, 0, memory_order_relaxed);
    atomic_store_explicit(&g_memprof.death_during_birth, 0, memory_order_relaxed);
    atomic_store_explicit(&g_memprof.zombie_races_detected, 0, memory_order_relaxed);
    atomic_store_explicit(&g_memprof.tombstones_recycled, 0, memory_order_relaxed);
    atomic_store_explicit(&g_memprof.shallow_stack_warnings, 0, memory_order_relaxed);
    
    /* Initialize data structures */
    if (heap_map_init() != 0) {
        return -1;
    }
    
    if (stack_table_init() != 0) {
        heap_map_destroy();
        return -1;
    }
    
    if (bloom_init() != 0) {
        stack_table_destroy();
        heap_map_destroy();
        return -1;
    }
    
    /* Register fork handlers (ignore failure - not critical) */
    (void)sampling_register_fork_handlers();
    
    /* Mark as initialized with release semantics to ensure all
     * previous writes are visible to other threads */
    atomic_store_explicit(&g_memprof.initialized, 1, memory_order_release);
    
    return 0;
}

/* ============================================================================
 * Start/Stop
 * ============================================================================ */

int memprof_start(void) {
    /* Check state */
    if (!atomic_load_explicit(&g_memprof.initialized, memory_order_acquire)) {
        /* Auto-init with defaults */
        if (memprof_init(0) != 0) {
            return -1;
        }
    }
    
    if (atomic_load_explicit(&g_memprof.shutdown, memory_order_relaxed)) {
        return -1;  /* Cannot restart after shutdown */
    }
    
    if (atomic_load_explicit(&g_memprof.active_alloc, memory_order_relaxed)) {
        return -1;  /* Already running */
    }
    
    /* Install platform-specific hooks */
    int result = 0;
#if defined(__APPLE__)
    result = memprof_darwin_install();
#elif defined(__linux__)
    result = memprof_linux_install();
#elif defined(_WIN32)
    result = memprof_windows_install();
#endif
    
    if (result != 0) {
        return -1;
    }
    
    /* Enable profiling */
    atomic_store_explicit(&g_memprof.active_free, 1, memory_order_relaxed);
    atomic_store_explicit(&g_memprof.active_alloc, 1, memory_order_release);
    
    return 0;
}

int memprof_stop(void) {
    /* Make stop() idempotent - safe to call multiple times */
    int was_running = atomic_exchange_explicit(&g_memprof.active_alloc, 0, 
                                                memory_order_acq_rel);
    
    if (!was_running) {
        return 0;  /* Already stopped - success (idempotent) */
    }
    
    /* Note: active_free remains 1 until shutdown to track frees
     * of allocations made during profiling */
    
    /* Resolve symbols if configured */
    if (g_memprof.resolve_on_stop) {
        memprof_resolve_symbols();
    }
    
    return 0;
}

/* ============================================================================
 * Snapshot
 * ============================================================================ */

/* Callback context for snapshot iteration */
typedef struct {
    HeapMapEntry* entries;
    size_t count;
    size_t capacity;
} SnapshotContext;

static void snapshot_callback(const HeapMapEntry* entry, void* user_data) {
    SnapshotContext* ctx = (SnapshotContext*)user_data;
    
    if (ctx->count >= ctx->capacity) {
        return;  /* Buffer full */
    }
    
    /* Copy entry */
    HeapMapEntry* out = &ctx->entries[ctx->count];
    out->ptr = atomic_load_explicit(&entry->ptr, memory_order_acquire);
    out->metadata = atomic_load_explicit(&entry->metadata, memory_order_relaxed);
    out->birth_seq = atomic_load_explicit(&entry->birth_seq, memory_order_relaxed);
    out->timestamp = entry->timestamp;
    
    ctx->count++;
}

int memprof_get_snapshot(HeapMapEntry** out_entries, size_t* out_count) {
    if (!out_entries || !out_count) {
        return -1;
    }
    
    *out_entries = NULL;
    *out_count = 0;
    
    if (!g_memprof.heap_map) {
        return -1;
    }
    
    /* Estimate capacity based on current insertions - deletions */
    uint64_t insertions = atomic_load_explicit(&g_memprof.heap_map_insertions,
                                                memory_order_relaxed);
    uint64_t deletions = atomic_load_explicit(&g_memprof.heap_map_deletions,
                                               memory_order_relaxed);
    
    size_t estimated = (insertions > deletions) ? 
                       (size_t)(insertions - deletions) : 0;
    
    /* Add some buffer for concurrent operations */
    size_t capacity = estimated + 1000;
    if (capacity > MEMPROF_HEAP_MAP_CAPACITY) {
        capacity = MEMPROF_HEAP_MAP_CAPACITY;
    }
    
    /* Allocate output buffer */
    HeapMapEntry* entries = (HeapMapEntry*)malloc(capacity * sizeof(HeapMapEntry));
    if (!entries) {
        return -1;
    }
    
    /* Iterate and collect live entries */
    SnapshotContext ctx = {
        .entries = entries,
        .count = 0,
        .capacity = capacity
    };
    
    heap_map_iterate(snapshot_callback, &ctx);
    
    *out_entries = entries;
    *out_count = ctx.count;
    
    return 0;
}

void memprof_free_snapshot(HeapMapEntry* entries) {
    free(entries);
}

/* ============================================================================
 * Statistics
 * ============================================================================ */

int memprof_get_stats(MemProfStats* out) {
    if (!out) {
        return -1;
    }
    
    /* Check if profiler is initialized */
    if (!atomic_load_explicit(&g_memprof.initialized, memory_order_acquire)) {
        memset(out, 0, sizeof(*out));
        return -1;
    }
    
    memset(out, 0, sizeof(*out));
    
    out->total_samples = atomic_load_explicit(&g_memprof.total_samples, memory_order_relaxed);
    out->freed_samples = atomic_load_explicit(&g_memprof.total_frees_tracked, memory_order_relaxed);
    out->live_samples = (out->total_samples > out->freed_samples) ?
                        (out->total_samples - out->freed_samples) : 0;
    
    out->unique_stacks = stack_table_count();
    out->sampling_rate_bytes = g_memprof.sampling_rate;
    
    /* Estimate heap size: sum of weights for live entries */
    /* For simplicity, use live_samples * weight (average) */
    out->estimated_heap_bytes = out->live_samples * g_memprof.sampling_rate;
    
    out->heap_map_load_percent = (float)heap_map_load_percent();
    
    out->collisions = atomic_load_explicit(&g_memprof.heap_map_collisions, memory_order_relaxed) +
                      atomic_load_explicit(&g_memprof.stack_table_collisions, memory_order_relaxed);
    
    out->shallow_stack_warnings = atomic_load_explicit(&g_memprof.shallow_stack_warnings,
                                                        memory_order_relaxed);
    out->death_during_birth = atomic_load_explicit(&g_memprof.death_during_birth,
                                                    memory_order_relaxed);
    out->zombie_races_detected = atomic_load_explicit(&g_memprof.zombie_races_detected,
                                                       memory_order_relaxed);
    
    return 0;
}

/* ============================================================================
 * Symbol Resolution
 * ============================================================================ */

int memprof_resolve_symbols(void) {
    if (!g_memprof.stack_table) {
        return 0;
    }
    
    int resolved = 0;
    size_t capacity = g_memprof.stack_table_capacity;
    
    for (size_t i = 0; i < capacity; i++) {
        StackEntry* entry = &g_memprof.stack_table[i];
        
        /* Check if slot is occupied */
        uint64_t hash = atomic_load_explicit(&entry->hash, memory_order_relaxed);
        if (hash == 0) {
            continue;
        }
        
        /* Check if already resolved */
        if (entry->flags & STACK_FLAG_RESOLVED) {
            continue;
        }
        
        /* Resolve this stack */
        if (resolve_stack_entry(entry) == 0) {
            resolved++;
        }
    }
    
    return resolved;
}

/* ============================================================================
 * Shutdown
 * ============================================================================ */

void memprof_shutdown(void) {
    /* Disable all hooks first */
    atomic_store_explicit(&g_memprof.active_alloc, 0, memory_order_release);
    atomic_store_explicit(&g_memprof.active_free, 0, memory_order_release);
    atomic_store_explicit(&g_memprof.shutdown, 1, memory_order_release);
    
    /* Remove platform hooks */
#if defined(__APPLE__)
    memprof_darwin_remove();
#elif defined(__linux__)
    memprof_linux_remove();
#elif defined(_WIN32)
    memprof_windows_remove();
#endif
    
    /* Clean up Bloom filter leaked buffers */
    bloom_cleanup_leaked_filters();
    
    /* Note: We intentionally do NOT free heap_map and stack_table here.
     * This is a safety measure - there could be in-flight hooks that
     * haven't finished yet. The memory will be reclaimed by the OS
     * when the process exits.
     *
     * For testing purposes, if you need to fully clean up, call
     * the _destroy functions directly after ensuring no threads
     * are accessing the profiler.
     */
    
    atomic_store_explicit(&g_memprof.initialized, 0, memory_order_release);
}

