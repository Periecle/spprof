/**
 * resolver.h - Symbol resolution from raw frame pointers
 *
 * The resolver consumes samples from the ring buffer and resolves
 * raw PyCodeObject* pointers into human-readable function names,
 * filenames, and line numbers.
 *
 * Unlike the signal handler, the resolver runs in a safe context
 * and may acquire the Python GIL, allocate memory, etc.
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
 */
typedef struct {
    char function_name[SPPROF_MAX_FUNC_NAME];  /* Python function name (co_name) */
    char filename[SPPROF_MAX_FILENAME];        /* Source file path (co_filename) */
    int lineno;                                 /* Line number */
    int is_native;                              /* True if C extension shim frame */
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
 * This starts the consumer thread that reads from the ring buffer
 * and resolves symbols.
 *
 * @param rb Ring buffer to consume from.
 * @return 0 on success, -1 on error.
 */
int resolver_init(RingBuffer* rb);

/**
 * Shutdown the resolver subsystem.
 *
 * This stops the consumer thread and cleans up resources.
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
 * @param code_addr Raw PyCodeObject* pointer.
 * @param out Output frame info.
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
 */
void resolver_clear_cache(void);

/**
 * Get resolver statistics.
 *
 * @param cache_hits Number of cache hits.
 * @param cache_misses Number of cache misses.
 * @param invalid_frames Number of frames that couldn't be resolved.
 */
void resolver_get_stats(uint64_t* cache_hits, uint64_t* cache_misses, uint64_t* invalid_frames);

#endif /* SPPROF_RESOLVER_H */


