/* SPDX-License-Identifier: MIT
 * bloom.h - Bloom filter for free() hot path optimization
 *
 * 99.99% of frees are for non-sampled allocations. Without optimization,
 * every free requires a hash table probe (~15ns cache miss). The Bloom
 * filter provides O(1) definite-no answers with 0% false negatives.
 *
 * Parameters:
 * - 1M bits = 128 KB (fits in L2 cache)
 * - 4 hash functions (optimal for our load factor)
 * - ~2% false positive rate at 50K live entries
 * - Result: ~3ns average free path vs ~15ns without filter
 */

#ifndef SPPROF_BLOOM_H
#define SPPROF_BLOOM_H

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

