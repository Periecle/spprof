/**
 * ringbuffer.h - Lock-free ring buffer for async-signal-safe sample capture
 *
 * This is the core data structure that enables safe communication between
 * the signal handler (producer) and the resolver thread (consumer).
 *
 * CRITICAL: ringbuffer_write() MUST be async-signal-safe.
 */

#ifndef SPPROF_RINGBUFFER_H
#define SPPROF_RINGBUFFER_H

#include <stdint.h>
#include <stdatomic.h>
#include <stddef.h>

/* Ring buffer configuration */
#define SPPROF_RING_SIZE 65536      /* Power of 2 for fast modulo */
#define SPPROF_MAX_STACK_DEPTH 128  /* Maximum call stack depth */

/**
 * RawFrameData - Per-frame data captured in signal handler context
 *
 * Contains both the code object pointer and instruction pointer for
 * accurate line number resolution.
 */
typedef struct {
    uintptr_t code_ptr;     /* Raw PyCodeObject* pointer */
    uintptr_t instr_ptr;    /* Instruction pointer within bytecode */
} RawFrameData;

/**
 * RawSample - Captured in signal handler context
 *
 * Contains only raw pointers and integers - no strings or Python objects.
 * This struct is fixed-size to enable pre-allocation.
 */
typedef struct {
    uint64_t timestamp;                          /* Monotonic clock value (nanoseconds) */
    uint64_t thread_id;                          /* OS thread ID */
    int depth;                                   /* Number of valid frame pointers */
    int _padding;                                /* Alignment padding */
    uintptr_t frames[SPPROF_MAX_STACK_DEPTH];   /* Raw PyCodeObject* pointers (unresolved) */
    uintptr_t instr_ptrs[SPPROF_MAX_STACK_DEPTH]; /* Instruction pointers for line resolution */
} RawSample;

/**
 * RingBuffer - Lock-free SPSC (single-producer single-consumer) queue
 *
 * The signal handler is the single producer; the resolver thread is
 * the single consumer. Memory ordering uses acquire/release semantics.
 *
 * Invariants:
 *   - (write_idx - read_idx) <= capacity (never overwrite unread)
 *   - Overflow drops samples rather than blocking
 */
typedef struct {
    _Atomic uint64_t write_idx;                  /* Next write position (producer) */
    _Atomic uint64_t read_idx;                   /* Next read position (consumer) */
    _Atomic uint64_t dropped_count;              /* Samples dropped due to overflow */
    size_t capacity;                             /* Buffer capacity (power of 2) */
    size_t capacity_mask;                        /* capacity - 1 for fast modulo */
    RawSample* samples;                          /* Dynamically allocated sample slots */
    RawSample inline_samples[SPPROF_RING_SIZE];  /* Default inline storage */
} RingBuffer;

/**
 * Allocate and initialize a ring buffer with default size.
 *
 * Thread safety: NOT thread-safe. Call once at module init.
 * Async-signal safety: NO.
 *
 * @return Pointer to allocated RingBuffer, or NULL on failure.
 */
RingBuffer* ringbuffer_create(void);

/**
 * Allocate a ring buffer with custom memory limit.
 *
 * Calculates the maximum number of samples that fit in the memory limit
 * and creates a buffer of appropriate size (rounded down to power of 2).
 *
 * Thread safety: NOT thread-safe. Call once at module init.
 * Async-signal safety: NO.
 *
 * @param memory_limit_mb Maximum memory to use in megabytes.
 * @return Pointer to allocated RingBuffer, or NULL on failure.
 */
RingBuffer* ringbuffer_create_with_limit(size_t memory_limit_mb);

/**
 * Get the capacity (maximum samples) of the ring buffer.
 *
 * @param rb Ring buffer to query.
 * @return Maximum number of samples the buffer can hold.
 */
size_t ringbuffer_capacity(RingBuffer* rb);

/**
 * Free a ring buffer.
 *
 * Thread safety: NOT thread-safe. Call once at module cleanup.
 * Async-signal safety: NO.
 *
 * @param rb Ring buffer to free.
 */
void ringbuffer_destroy(RingBuffer* rb);

/**
 * Write a sample to the ring buffer.
 *
 * Thread safety: Single-producer safe.
 * Async-signal safety: YES - this is called from the signal handler.
 *
 * If the buffer is full, the sample is dropped and dropped_count is
 * incremented. This never blocks.
 *
 * @param rb Ring buffer to write to.
 * @param sample Sample to write (copied into buffer).
 * @return 1 on success, 0 if buffer full (sample dropped).
 */
int ringbuffer_write(RingBuffer* rb, const RawSample* sample);

/**
 * Read a sample from the ring buffer.
 *
 * Thread safety: Single-consumer safe.
 * Async-signal safety: NO.
 *
 * @param rb Ring buffer to read from.
 * @param out Output buffer to copy sample into.
 * @return 1 if sample read, 0 if buffer empty.
 */
int ringbuffer_read(RingBuffer* rb, RawSample* out);

/**
 * Check if buffer has data available for reading.
 *
 * Thread safety: Thread-safe (atomic read).
 * Async-signal safety: YES.
 *
 * @param rb Ring buffer to check.
 * @return 1 if data available, 0 if empty.
 */
int ringbuffer_has_data(RingBuffer* rb);

/**
 * Get the number of dropped samples.
 *
 * Thread safety: Thread-safe (atomic read).
 * Async-signal safety: YES.
 *
 * @param rb Ring buffer to query.
 * @return Number of samples dropped due to buffer overflow.
 */
uint64_t ringbuffer_dropped_count(RingBuffer* rb);

/**
 * Reset the ring buffer to empty state.
 *
 * Thread safety: NOT thread-safe. Call only when profiler is stopped.
 * Async-signal safety: NO.
 *
 * @param rb Ring buffer to reset.
 */
void ringbuffer_reset(RingBuffer* rb);

#endif /* SPPROF_RINGBUFFER_H */


