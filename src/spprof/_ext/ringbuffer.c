/**
 * ringbuffer.c - Lock-free ring buffer implementation
 *
 * Implementation of the SPSC (single-producer single-consumer) ring buffer
 * for async-signal-safe sample capture.
 *
 * Memory ordering:
 *   - Producer uses release semantics when publishing
 *   - Consumer uses acquire semantics when reading
 *   - This ensures proper visibility of sample data
 */

#include "ringbuffer.h"
#include <stdlib.h>
#include <string.h>

/*
 * Platform-specific atomic operations
 *
 * Windows uses Interlocked functions; POSIX uses C11 atomics.
 * Memory barriers are provided by the Interlocked functions on Windows.
 */
#ifdef _WIN32

/* Windows atomic operations using Interlocked functions */
#define ATOMIC_INIT(ptr, val) (*(ptr) = (val))
#define ATOMIC_LOAD_RELAXED(ptr) InterlockedCompareExchange64(ptr, 0, 0)
#define ATOMIC_LOAD_ACQUIRE(ptr) InterlockedCompareExchange64(ptr, 0, 0)
#define ATOMIC_STORE_RELAXED(ptr, val) InterlockedExchange64(ptr, val)
#define ATOMIC_STORE_RELEASE(ptr, val) InterlockedExchange64(ptr, val)
#define ATOMIC_FETCH_ADD_RELAXED(ptr, val) InterlockedExchangeAdd64(ptr, val)

#else

/* POSIX uses C11 atomics */
#define ATOMIC_INIT(ptr, val) atomic_init(ptr, val)
#define ATOMIC_LOAD_RELAXED(ptr) atomic_load_explicit(ptr, memory_order_relaxed)
#define ATOMIC_LOAD_ACQUIRE(ptr) atomic_load_explicit(ptr, memory_order_acquire)
#define ATOMIC_STORE_RELAXED(ptr, val) atomic_store_explicit(ptr, val, memory_order_relaxed)
#define ATOMIC_STORE_RELEASE(ptr, val) atomic_store_explicit(ptr, val, memory_order_release)
#define ATOMIC_FETCH_ADD_RELAXED(ptr, val) atomic_fetch_add_explicit(ptr, val, memory_order_relaxed)

#endif

/**
 * Round up to the nearest power of 2.
 */
static size_t next_power_of_2(size_t n) {
    if (n == 0) return 1;
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
#if SIZE_MAX > 0xFFFFFFFF
    n |= n >> 32;
#endif
    return n + 1;
}

RingBuffer* ringbuffer_create(void) {
    /* Allocate the ring buffer structure */
    RingBuffer* rb = (RingBuffer*)calloc(1, sizeof(RingBuffer));
    if (rb == NULL) {
        return NULL;
    }

    /* Initialize atomic indices */
    ATOMIC_INIT(&rb->write_idx, 0);
    ATOMIC_INIT(&rb->read_idx, 0);
    ATOMIC_INIT(&rb->dropped_count, 0);
    rb->capacity = SPPROF_RING_SIZE;
    rb->capacity_mask = SPPROF_RING_SIZE - 1;
    
    /* Use inline storage by default */
    rb->samples = rb->inline_samples;

    /* Samples are already zeroed by calloc */
    return rb;
}

RingBuffer* ringbuffer_create_with_limit(size_t memory_limit_mb) {
    /* Calculate how many samples fit in the memory limit */
    size_t memory_bytes = memory_limit_mb * 1024 * 1024;
    size_t sample_size = sizeof(RawSample);
    size_t max_samples = memory_bytes / sample_size;
    
    /* Minimum 1024 samples, maximum SPPROF_RING_SIZE for inline */
    if (max_samples < 1024) {
        max_samples = 1024;
    }
    
    /* Round down to power of 2 */
    size_t capacity = next_power_of_2(max_samples);
    if (capacity > max_samples) {
        capacity /= 2;
    }
    
    /* If it fits in default size, use inline storage */
    if (capacity <= SPPROF_RING_SIZE) {
        RingBuffer* rb = ringbuffer_create();
        if (rb != NULL) {
            rb->capacity = capacity;
            rb->capacity_mask = capacity - 1;
        }
        return rb;
    }
    
    /* Allocate ring buffer with external storage */
    RingBuffer* rb = (RingBuffer*)calloc(1, sizeof(RingBuffer));
    if (rb == NULL) {
        return NULL;
    }
    
    /* Allocate sample storage */
    rb->samples = (RawSample*)calloc(capacity, sizeof(RawSample));
    if (rb->samples == NULL) {
        free(rb);
        return NULL;
    }
    
    /* Initialize atomic indices */
    ATOMIC_INIT(&rb->write_idx, 0);
    ATOMIC_INIT(&rb->read_idx, 0);
    ATOMIC_INIT(&rb->dropped_count, 0);
    rb->capacity = capacity;
    rb->capacity_mask = capacity - 1;
    
    return rb;
}

size_t ringbuffer_capacity(RingBuffer* rb) {
    return rb ? rb->capacity : 0;
}

void ringbuffer_destroy(RingBuffer* rb) {
    if (rb != NULL) {
        /* Free external sample storage if not using inline */
        if (rb->samples != NULL && rb->samples != rb->inline_samples) {
            free(rb->samples);
        }
        free(rb);
    }
}

/*
 * ringbuffer_write - Async-signal-safe write operation
 *
 * CRITICAL: This function is called from the signal handler.
 * It MUST NOT:
 *   - Call malloc/free
 *   - Acquire locks
 *   - Call non-async-signal-safe functions
 */
int ringbuffer_write(RingBuffer* rb, const RawSample* sample) {
    /* Read current write position (relaxed - single producer) */
    uint64_t write_pos = ATOMIC_LOAD_RELAXED(&rb->write_idx);

    /* Calculate next position */
    uint64_t next_pos = write_pos + 1;

    /* Check for overflow: would we overwrite unread data? */
    uint64_t read_pos = ATOMIC_LOAD_ACQUIRE(&rb->read_idx);
    if (next_pos - read_pos > rb->capacity) {
        /* Buffer full - drop sample */
        ATOMIC_FETCH_ADD_RELAXED(&rb->dropped_count, 1);
        return 0;
    }

    /* Get slot using bitmask (fast modulo for power-of-2) */
    uint64_t slot_idx = write_pos & rb->capacity_mask;
    RawSample* slot = &rb->samples[slot_idx];

    /* Copy sample data into slot */
    /* memcpy is generally async-signal-safe for simple copies */
    slot->timestamp = sample->timestamp;
    slot->thread_id = sample->thread_id;
    slot->depth = sample->depth;
    slot->_padding = 0;

    /* Copy frame pointers and instruction pointers */
    for (int i = 0; i < sample->depth && i < SPPROF_MAX_STACK_DEPTH; i++) {
        slot->frames[i] = sample->frames[i];
        slot->instr_ptrs[i] = sample->instr_ptrs[i];
    }

    /* Publish: make the sample visible to consumer */
    ATOMIC_STORE_RELEASE(&rb->write_idx, next_pos);

    return 1;
}

int ringbuffer_read(RingBuffer* rb, RawSample* out) {
    /* Read current read position */
    uint64_t read_pos = ATOMIC_LOAD_RELAXED(&rb->read_idx);

    /* Check if buffer is empty */
    uint64_t write_pos = ATOMIC_LOAD_ACQUIRE(&rb->write_idx);
    if (read_pos >= write_pos) {
        return 0;  /* Empty */
    }

    /* Get slot */
    uint64_t slot_idx = read_pos & rb->capacity_mask;
    RawSample* slot = &rb->samples[slot_idx];

    /* Copy sample data to output */
    out->timestamp = slot->timestamp;
    out->thread_id = slot->thread_id;
    out->depth = slot->depth;
    out->_padding = 0;

    for (int i = 0; i < slot->depth && i < SPPROF_MAX_STACK_DEPTH; i++) {
        out->frames[i] = slot->frames[i];
        out->instr_ptrs[i] = slot->instr_ptrs[i];
    }

    /* Advance read position */
    ATOMIC_STORE_RELEASE(&rb->read_idx, read_pos + 1);

    return 1;
}

int ringbuffer_has_data(RingBuffer* rb) {
    uint64_t read_pos = ATOMIC_LOAD_ACQUIRE(&rb->read_idx);
    uint64_t write_pos = ATOMIC_LOAD_ACQUIRE(&rb->write_idx);
    return write_pos > read_pos;
}

uint64_t ringbuffer_dropped_count(RingBuffer* rb) {
    return ATOMIC_LOAD_RELAXED(&rb->dropped_count);
}

void ringbuffer_reset(RingBuffer* rb) {
    ATOMIC_STORE_RELAXED(&rb->write_idx, 0);
    ATOMIC_STORE_RELAXED(&rb->read_idx, 0);
    ATOMIC_STORE_RELAXED(&rb->dropped_count, 0);
}


