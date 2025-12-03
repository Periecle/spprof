/* SPDX-License-Identifier: MIT
 * sampling.h - Poisson sampling engine
 *
 * This implements Poisson sampling with exponential inter-sample intervals.
 * The key insight is that sampling probability is proportional to allocation
 * size, making large allocations more likely to be captured.
 *
 * Hot path (99.99% of calls):
 *   - TLS access (~1-2 cycles)
 *   - Single subtract (1 cycle)
 *   - Single compare + branch (1 cycle)
 *   - Total: ~5-10 cycles
 *
 * Cold path (sampling):
 *   - Stack capture (~50-100 cycles)
 *   - Hash + intern (~50 cycles)
 *   - Heap map insert (~50 cycles)
 *   - PRNG + threshold (~10 cycles)
 *   - Total: ~500-2000 cycles
 */

#ifndef SPPROF_SAMPLING_H
#define SPPROF_SAMPLING_H

#include "memprof.h"
#include <stdint.h>

/* ============================================================================
 * Thread-Local State Access
 * ============================================================================ */

/**
 * Get or initialize thread-local sampler state.
 * 
 * @return Pointer to current thread's MemProfThreadState
 */
MemProfThreadState* sampling_get_tls(void);

/**
 * Ensure TLS is initialized for current thread.
 * Called at start of each allocation hook.
 */
void sampling_ensure_tls_init(void);

/* ============================================================================
 * PRNG (xorshift128+)
 * ============================================================================ */

/**
 * Generate next 64-bit random number.
 * 
 * Properties:
 *   - Period: 2^128 - 1
 *   - Speed: ~1.5 cycles per call
 *   - Quality: Passes BigCrush
 *
 * @param state  PRNG state array (modified in place)
 * @return 64-bit random value
 */
uint64_t prng_next(uint64_t state[2]);

/**
 * Generate uniform double in [0, 1).
 *
 * @param state  PRNG state array (modified in place)
 * @return Double in [0, 1)
 */
double prng_next_double(uint64_t state[2]);

/* ============================================================================
 * Threshold Generation
 * ============================================================================ */

/**
 * Generate next sampling threshold using exponential distribution.
 *
 * Mathematical basis: If X ~ Exponential(λ), then X = -ln(U)/λ
 * where U ~ Uniform(0,1) and λ = 1/mean.
 *
 * @param state       PRNG state array (modified in place)
 * @param mean_bytes  Average bytes between samples
 * @return Threshold in bytes (always positive)
 */
int64_t next_sample_threshold(uint64_t state[2], uint64_t mean_bytes);

/* ============================================================================
 * Hot Path Functions
 * ============================================================================ */

/**
 * Check if this allocation should be sampled.
 *
 * This is the HOT PATH - must be as fast as possible.
 * Decrements byte counter and checks if <= 0.
 *
 * @param size  Allocation size in bytes
 * @return 1 if should sample, 0 otherwise
 */
static inline int sampling_should_sample(MemProfThreadState* tls, size_t size) {
    tls->byte_counter -= (int64_t)size;
    return tls->byte_counter <= 0;
}

/**
 * Reset the sampling threshold after sampling.
 *
 * @param tls  Thread-local state
 */
void sampling_reset_threshold(MemProfThreadState* tls);

/* ============================================================================
 * Cold Path Functions
 * ============================================================================ */

/**
 * Handle a sampled allocation (cold path).
 *
 * This is called when byte_counter <= 0. It:
 * 1. Sets re-entrancy guard
 * 2. Captures stack trace
 * 3. Interns the stack
 * 4. Inserts into heap map
 * 5. Adds to Bloom filter
 * 6. Resets threshold
 * 7. Clears re-entrancy guard
 *
 * @param ptr   Allocated pointer
 * @param size  Allocation size in bytes
 */
void sampling_handle_sample(void* ptr, size_t size);

/**
 * Handle a free() call.
 *
 * Fast path: Check Bloom filter first.
 * If maybe sampled: Look up and remove from heap map.
 *
 * @param ptr  Freed pointer
 */
void sampling_handle_free(void* ptr);

/* ============================================================================
 * Fork Safety
 * ============================================================================ */

/**
 * Register pthread_atfork handlers for fork safety.
 *
 * @return 0 on success, -1 on error
 */
int sampling_register_fork_handlers(void);

/**
 * Check if we're in a forked child process.
 * Used for vfork safety - disables profiler in children.
 *
 * @return 1 if in forked child, 0 otherwise
 */
int sampling_in_forked_child(void);

#endif /* SPPROF_SAMPLING_H */

