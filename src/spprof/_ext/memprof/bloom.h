/* SPDX-License-Identifier: MIT
 * bloom.h - Bloom filter for free() hot path optimization
 *
 * REBUILD RACE FIX (2024):
 *   During bloom_rebuild_from_heap(), there's a race where new allocations
 *   can be missed if they occur after the iterator passes their slot but
 *   before the filter swap. This causes "ghost leaks" - entries in heap_map
 *   that never get removed because bloom_might_contain() returns false.
 *
 *   Solution: "Double Insert" strategy. When rebuild is in progress,
 *   bloom_add() writes to BOTH the old and new filters. This ensures
 *   no allocations are missed during the rebuild window.
 *
 * 99.99% of frees are for non-sampled allocations. Without optimization,
 * every free requires a hash table probe (~15ns cache miss). The Bloom
 * filter provides O(1) definite-no answers with 0% false negatives.
 *
 * PARAMETERS:
 *   - 1M bits = 128 KB (fits in L2 cache)
 *   - 4 hash functions (optimal for our load factor)
 *   - ~2% false positive rate at 50K live entries
 *   - Result: ~3ns average free path vs ~15ns without filter
 *
 * THREAD SAFETY:
 *   - bloom_add(): Uses atomic OR, thread-safe
 *   - bloom_might_contain(): Lock-free reads, thread-safe
 *   - bloom_rebuild_from_heap(): Single-writer with atomic swap
 *
 * SATURATION HANDLING:
 *   When filter exceeds 50% saturation, rebuild from live heap entries.
 *   Old filter is intentionally leaked during rebuild for use-after-free
 *   safety; cleaned up at shutdown via bloom_cleanup_leaked_filters().
 *
 * PLATFORM SUPPORT:
 *   - Linux/macOS: mmap for backing memory
 *   - Windows: VirtualAlloc
 *
 * Copyright (c) 2024 spprof contributors
 */

#ifndef SPPROF_BLOOM_H
#define SPPROF_BLOOM_H

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
 * Bloom Filter API
 * ============================================================================ */

/**
 * Initialize the Bloom filter.
 * Uses mmap to allocate backing array.
 *
 * @return 0 on success, -1 on error
 */
int bloom_init(void);

/**
 * Add pointer to Bloom filter.
 *
 * Uses atomic OR for thread safety.
 * 
 * RACE FIX (2024): Includes retry loop to handle the race where a rebuild
 * completes between loading active_filter and checking staging_filter.
 * This prevents "ghost leaks" where allocations are in heap_map but not
 * in the bloom filter.
 *
 * @param ptr  Pointer to add
 */
void bloom_add(uintptr_t ptr);

/**
 * Check if pointer MIGHT be in set.
 *
 * @param ptr  Pointer to check
 * @return 0 = definitely NOT sampled (fast path)
 *         1 = maybe sampled (check heap map)
 */
int bloom_might_contain(uintptr_t ptr);

/**
 * Check if the Bloom filter needs rebuilding.
 *
 * @return 1 if saturation > 50%, 0 otherwise
 */
int bloom_needs_rebuild(void);

/**
 * Get current saturation level.
 *
 * @return Approximate percentage of bits set (0-100)
 */
int bloom_saturation_percent(void);

/**
 * Rebuild Bloom filter from live heap map (background task).
 *
 * Called when saturation exceeds threshold. Steps:
 * 1. Allocate clean filter
 * 2. Iterate heap map, add all live pointers
 * 3. Atomic swap filter pointer
 * 4. Record old filter for cleanup at shutdown
 *
 * Note: Intentionally leaks old filter for safety (no use-after-free risk).
 *
 * @return 0 on success, -1 on error
 */
int bloom_rebuild_from_heap(void);

/**
 * Cleanup all leaked filters.
 * Only safe to call at shutdown after all threads have stopped.
 */
void bloom_cleanup_leaked_filters(void);

/**
 * Free Bloom filter resources.
 */
void bloom_destroy(void);

/* ============================================================================
 * Internal Helpers (exposed for testing)
 * ============================================================================ */

/**
 * Compute hash indices for a pointer.
 *
 * Uses double-hashing: h(i) = h1 + i*h2
 *
 * @param ptr      Pointer to hash
 * @param indices  Output array of BLOOM_HASH_COUNT indices
 */
void bloom_get_indices(uintptr_t ptr, uint64_t indices[BLOOM_HASH_COUNT]);

#endif /* SPPROF_BLOOM_H */

