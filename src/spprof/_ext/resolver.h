/**
 * resolver.h - Symbol resolution from raw frame pointers
 *
 * The resolver consumes samples from the ring buffer and resolves
 * raw PyCodeObject* pointers into human-readable function names,
 * filenames, and line numbers.
 *
 * Unlike the signal handler, the resolver runs in a safe context
 * and may acquire the Python GIL, allocate memory, etc.
 *
 * THREAD SAFETY:
 *
 * The resolver module uses an internal symbol resolution cache to avoid
 * redundant Python object accesses. This cache is protected by a mutex,
 * making the following functions safe to call from multiple threads:
 *
 *   - resolver_drain_samples()   (streaming API)
 *   - resolver_resolve_frame()
 *   - resolver_resolve_frame_with_line()
 *   - resolver_clear_cache()
 *   - resolver_get_stats()
 *
 * The following functions are NOT thread-safe and should only be called
 * from a single control thread:
 *
 *   - resolver_init()            (call once at startup)
 *   - resolver_shutdown()        (call once at cleanup)
 *   - resolver_get_samples()     (legacy API, accumulates all samples)
 *   - resolver_free_samples()    (must match resolver_get_samples caller)
 *
 * For the streaming API (resolver_drain_samples), multiple threads may
 * safely drain samples concurrently. Each call returns an independent
 * array that the caller must free.
 *
 * ERROR HANDLING CONVENTIONS (see error.h for full documentation):
 *
 *   Pattern 1 - POSIX-style (0 success, -1 error):
 *     - resolver_init()
 *     - resolver_get_samples()
 *     - resolver_drain_samples()
 *
 *   Pattern 2 - Boolean success (1 success, 0 failure):
 *     - resolver_resolve_frame()
 *     - resolver_resolve_frame_with_line()
 *     - resolver_has_pending_samples()
 */

#ifndef SPPROF_RESOLVER_H
#define SPPROF_RESOLVER_H

#include <stdint.h>
#include <stddef.h>
#include "ringbuffer.h"

/* Maximum lengths for resolved strings */
#define SPPROF_MAX_FUNC_NAME 256
#define SPPROF_MAX_FILENAME 1024

/**
 * ResolvedFrame - A frame with resolved symbol information
 *
 * For Python frames:
 *   - function_name: Python function name (from co_name)
 *   - filename: Source file path (from co_filename)
 *   - lineno: Source line number
 *   - is_native: 0
 *
 * For Native frames (resolved via dladdr):
 *   - function_name: C function name (e.g., "sin", "deflate")
 *   - filename: Library path (e.g., "/usr/lib/libz.dylib")
 *   - lineno: 0 (no line info for native)
 *   - is_native: 1
 */
typedef struct {
    char function_name[SPPROF_MAX_FUNC_NAME];  /* Function name (Python or C) */
    char filename[SPPROF_MAX_FILENAME];        /* Source file or library path */
    int lineno;                                 /* Line number (0 for native) */
    int is_native;                              /* 1 if native C frame, 0 if Python */
} ResolvedFrame;

/**
 * ResolvedSample - A fully resolved sample ready for output
 */
typedef struct {
    ResolvedFrame frames[SPPROF_MAX_STACK_DEPTH];  /* Resolved stack frames */
    int depth;                                      /* Number of valid frames */
    uint64_t timestamp;                             /* Original timestamp (ns) */
    uint64_t thread_id;                             /* Thread ID */
} ResolvedSample;

/**
 * Initialize the resolver subsystem.
 *
 * This initializes the symbol resolution cache and prepares for
 * resolving samples from the ring buffer.
 *
 * Thread safety: NOT thread-safe. Call once from control thread.
 *
 * Error handling: POSIX-style (Pattern 1)
 *   Returns 0 on success
 *   Returns -1 on error (memory allocation failure)
 *
 * @param rb Ring buffer to consume from.
 * @return 0 on success, -1 on error.
 */
int resolver_init(RingBuffer* rb);

/**
 * Shutdown the resolver subsystem.
 *
 * This cleans up the cache and releases resources.
 *
 * Thread safety: NOT thread-safe. Call once from control thread.
 * Ensure no other threads are calling resolver functions.
 */
void resolver_shutdown(void);

/**
 * Get all resolved samples.
 *
 * Call this after profiling stops to retrieve all resolved samples.
 * The caller is responsible for freeing the returned array with
 * resolver_free_samples().
 *
 * @param out Pointer to receive the sample array.
 * @param count Pointer to receive the sample count.
 * @return 0 on success, -1 on error.
 */
int resolver_get_samples(ResolvedSample** out, size_t* count);

/**
 * Free the resolved samples array.
 *
 * @param samples Array returned by resolver_get_samples().
 * @param count Number of samples in the array.
 */
void resolver_free_samples(ResolvedSample* samples, size_t count);

/**
 * Resolve a single code object pointer to frame info.
 *
 * This function acquires the GIL briefly to access Python objects.
 *
 * Thread safety: SAFE to call from multiple threads concurrently.
 * The internal cache is protected by a mutex.
 *
 * Error handling: Boolean success (Pattern 2)
 *   Returns 1 = success (frame resolved, output populated)
 *   Returns 0 = failure (code object invalid or NULL)
 *
 * @param code_addr Raw PyCodeObject* pointer.
 * @param out Output frame info (populated on success).
 * @return 1 if resolved successfully, 0 if code object invalid.
 */
int resolver_resolve_frame(uintptr_t code_addr, ResolvedFrame* out);

/**
 * Resolve a frame with instruction pointer for accurate line numbers.
 *
 * This function uses the instruction pointer to compute the exact line
 * being executed, rather than the first line of the function.
 *
 * @param code_addr Raw PyCodeObject* pointer.
 * @param instr_ptr Instruction pointer within the code object.
 * @param out Output frame info with accurate lineno.
 * @return 1 if resolved successfully, 0 if code object invalid.
 */
int resolver_resolve_frame_with_line(uintptr_t code_addr, uintptr_t instr_ptr, ResolvedFrame* out);

/**
 * Clear the symbol resolution cache.
 *
 * Thread safety: SAFE to call from multiple threads concurrently.
 */
void resolver_clear_cache(void);

/**
 * Get resolver statistics.
 *
 * Thread safety: SAFE to call from multiple threads concurrently.
 * Statistics are read atomically under the cache lock.
 *
 * @param cache_hits Number of cache hits.
 * @param cache_misses Number of cache misses.
 * @param cache_collisions Number of LRU evictions (can be NULL).
 * @param invalid_frames Number of frames that couldn't be resolved.
 */
void resolver_get_stats(uint64_t* cache_hits, uint64_t* cache_misses, 
                        uint64_t* cache_collisions, uint64_t* invalid_frames);

/**
 * Drain samples from the ring buffer in chunks (streaming API).
 *
 * This function drains up to max_samples from the ring buffer, resolves them,
 * and returns them. This allows streaming results to disk without accumulating
 * all samples in memory, preventing OOM for long profiling sessions.
 *
 * The caller MUST free the returned array with free() when done.
 * Unlike resolver_get_samples(), each call returns a NEW array.
 *
 * Thread safety: SAFE to call from multiple threads concurrently. The internal
 * symbol resolution cache is protected by a mutex. Each call returns an
 * independent array that the caller owns.
 *
 * Error handling: POSIX-style (Pattern 1)
 *   Returns 0 on success (even if buffer empty - check *count)
 *   Returns -1 on error (memory allocation failure)
 *
 * @param max_samples Maximum number of samples to drain (0 = use default batch).
 * @param out Pointer to receive newly allocated sample array (caller frees).
 * @param count Pointer to receive the actual number of samples drained.
 * @return 0 on success, -1 on error. Returns 0 with *count=0 if buffer empty.
 */
int resolver_drain_samples(size_t max_samples, ResolvedSample** out, size_t* count);

/**
 * Check if there are more samples pending in the ring buffer.
 *
 * @return 1 if samples are available, 0 if buffer is empty.
 */
int resolver_has_pending_samples(void);

#endif /* SPPROF_RESOLVER_H */


