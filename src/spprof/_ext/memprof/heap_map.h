/* SPDX-License-Identifier: MIT
 * heap_map.h - Lock-free heap map for sampled allocations
 *
 * This implements a lock-free hash table using open addressing with linear
 * probing. The key insight is a two-phase insert (reserve→finalize) that
 * prevents the "free-before-insert" race condition.
 *
 * STATE MACHINE:
 *   EMPTY → RESERVED (malloc: CAS success)
 *   TOMBSTONE → RESERVED (malloc: CAS success, recycling)
 *   RESERVED → ptr (malloc: finalize)
 *   RESERVED → TOMBSTONE (free: "death during birth")
 *   ptr → TOMBSTONE (free: normal path)
 *
 * THREAD SAFETY:
 *   All operations are lock-free using CAS and atomic loads/stores.
 *   Safe for concurrent access from multiple threads.
 *
 * MEMORY ORDERING:
 *   - reserve(): Uses acq_rel CAS to establish happens-before with finalize
 *   - finalize(): Uses release store to publish to readers
 *   - remove(): Uses acquire load to see finalized data
 *   - lookup(): Uses acquire load for consistent reads
 *
 * ERROR HANDLING:
 *   - reserve() returns -1 if table is full
 *   - finalize() returns 0 if "death during birth" occurred
 *   - remove() returns 0 if not found (including zombie detection)
 *
 * Copyright (c) 2024 spprof contributors
 */

#ifndef SPPROF_HEAP_MAP_H
#define SPPROF_HEAP_MAP_H

/* _GNU_SOURCE for consistency with other memprof files */
#if defined(__linux__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#endif

#include "memprof.h"
#include <stdint.h>
#include <stdatomic.h>

/* ============================================================================
 * Heap Map API
 * ============================================================================ */

/**
 * Initialize the heap map.
 * Uses mmap to allocate backing array (avoids malloc recursion).
 * 
 * @return 0 on success, -1 on error
 */
int heap_map_init(void);

/**
 * Reserve a slot for a sampled allocation (Phase 1 of insert).
 *
 * Uses CAS to claim EMPTY or TOMBSTONE slot as RESERVED.
 * Stores ptr in metadata temporarily for matching during "death during birth".
 *
 * @param ptr  Allocated pointer address
 * @return Slot index on success, -1 if table full
 */
int heap_map_reserve(uintptr_t ptr);

/**
 * Finalize a reserved slot with metadata (Phase 2 of insert).
 *
 * CAS: RESERVED → ptr. If fails, "death during birth" occurred.
 *
 * @param slot_idx        Slot index from heap_map_reserve()
 * @param ptr             Allocated pointer
 * @param stack_id        Interned stack ID
 * @param size            Allocation size in bytes (full 64-bit)
 * @param weight          Sampling weight (full 32-bit)
 * @param birth_seq       Global sequence number at allocation time
 * @param timestamp       Monotonic timestamp in nanoseconds
 * @return 1 on success, 0 if "death during birth"
 */
int heap_map_finalize(int slot_idx, uintptr_t ptr, uint32_t stack_id,
                      uint64_t size, uint32_t weight, uint64_t birth_seq,
                      uint64_t timestamp);

/**
 * Remove a freed allocation from heap map.
 *
 * Handles both OCCUPIED → TOMBSTONE and RESERVED → TOMBSTONE transitions.
 * Uses sequence number to detect macOS ABA race (zombie killer).
 *
 * @param ptr             Freed pointer address
 * @param free_seq        Sequence number captured at free() entry
 * @param free_timestamp  Timestamp for duration calculation
 * @param out_stack_id    Output: stack ID of removed entry (optional)
 * @param out_size        Output: size of removed entry (optional, 64-bit)
 * @param out_weight      Output: weight of removed entry (optional)
 * @param out_duration    Output: lifetime in nanoseconds (optional)
 * @return 1 if found and removed, 0 if not found
 */
int heap_map_remove(uintptr_t ptr, uint64_t free_seq, uint64_t free_timestamp,
                    uint32_t* out_stack_id, uint64_t* out_size,
                    uint32_t* out_weight, uint64_t* out_duration);

/**
 * Look up a pointer in the heap map without modifying it.
 *
 * @param ptr  Pointer to look up
 * @return Pointer to entry if found, NULL otherwise
 */
const HeapMapEntry* heap_map_lookup(uintptr_t ptr);

/**
 * Get current load factor.
 *
 * @return Load factor as percentage (0-100)
 */
int heap_map_load_percent(void);

/**
 * Get count of live entries (OCCUPIED state).
 *
 * @return Number of live entries
 */
size_t heap_map_live_count(void);

/**
 * Iterate over all live entries in the heap map.
 *
 * @param callback  Function to call for each live entry
 * @param user_data User data passed to callback
 * @return Number of entries visited
 */
typedef void (*heap_map_iter_fn)(const HeapMapEntry* entry, void* user_data);
size_t heap_map_iterate(heap_map_iter_fn callback, void* user_data);

/**
 * Free heap map resources.
 * Only safe to call after all threads have stopped using the profiler.
 */
void heap_map_destroy(void);

/* ============================================================================
 * Internal Helpers (exposed for testing)
 * ============================================================================ */

/**
 * Hash a pointer to a heap map index.
 */
static inline uint64_t heap_map_hash_ptr(uintptr_t ptr) {
    /* Multiplicative hash with golden ratio constant */
    uint64_t h = (uint64_t)ptr;
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDULL;
    h ^= h >> 33;
    h *= 0xC4CEB9FE1A85EC53ULL;
    h ^= h >> 33;
    return h;
}

/**
 * Check if a ptr value represents a valid allocation (not a state marker).
 */
static inline int heap_map_is_valid_ptr(uintptr_t ptr) {
    return ptr != HEAP_ENTRY_EMPTY && 
           ptr != HEAP_ENTRY_RESERVED && 
           ptr != HEAP_ENTRY_TOMBSTONE;
}

#endif /* SPPROF_HEAP_MAP_H */

