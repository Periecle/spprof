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
 * Windows uses Windows 8+ SDK intrinsics for atomic operations.
 * POSIX uses C11 stdatomic.
 *
 * MINIMUM WINDOWS VERSION: Windows 8 / Windows Server 2012
 * Windows 7 and earlier are NOT supported.
 *
 * MEMORY ORDERING SEMANTICS:
 *
 * RELAXED: No ordering guarantees. Used when:
 *   - Reading a value that only the current thread writes (SPSC optimization)
 *   - The value doesn't synchronize other data
 *
 * ACQUIRE: Prevents reads/writes from being reordered before this load.
 *   Used by consumers to see all data published by the producer.
 *
 * RELEASE: Prevents reads/writes from being reordered after this store.
 *   Used by producers to publish data visible to consumers.
 *
 * WINDOWS 8+ SDK INTRINSICS:
 *   - ReadNoFence64(ptr)      - Relaxed load (no memory barrier)
 *   - WriteNoFence64(ptr,val) - Relaxed store (no memory barrier)
 *   - ReadAcquire64(ptr)      - Load with acquire semantics
 *   - WriteRelease64(ptr,val) - Store with release semantics
 *
 * These intrinsics compile to:
 *   - On x86/x64: Plain loads/stores (hardware TSO provides ordering)
 *   - On ARM64:   Appropriate ldar/stlr instructions for acquire/release
 */
#ifdef _WIN32

/*
 * Relaxed operations: No memory ordering guarantees.
 *
 * ReadNoFence64/WriteNoFence64 provide atomic access without barriers.
 * On x86/x64, these are plain volatile accesses.
 * On ARM64, these are plain loads/stores without acquire/release semantics.
 */
#define ATOMIC_INIT(ptr, val) (*(ptr) = (val))
#define ATOMIC_LOAD_RELAXED(ptr) ReadNoFence64((volatile LONG64*)(ptr))
#define ATOMIC_STORE_RELAXED(ptr, val) WriteNoFence64((volatile LONG64*)(ptr), (val))

/*
 * Acquire/Release operations: Provide synchronization guarantees.
 *
 * ReadAcquire64: Ensures all subsequent reads/writes happen after this load.
 * WriteRelease64: Ensures all prior reads/writes happen before this store.
 *
 * These pair together to form a "synchronizes-with" relationship:
 * Producer: write data, then WriteRelease64(flag)
 * Consumer: ReadAcquire64(flag), then read data
 */
#define ATOMIC_LOAD_ACQUIRE(ptr) ReadAcquire64((volatile LONG64*)(ptr))
#define ATOMIC_STORE_RELEASE(ptr, val) WriteRelease64((volatile LONG64*)(ptr), (val))

/*
 * Atomic fetch-add: Always requires full atomicity (RMW operation).
 * InterlockedExchangeAdd64 provides sequential consistency.
 */
#define ATOMIC_FETCH_ADD_RELAXED(ptr, val) InterlockedExchangeAdd64((ptr), (val))

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
    slot->native_depth = sample->native_depth;

    /* Copy Python frame pointers and instruction pointers */
    for (int i = 0; i < sample->depth && i < SPPROF_MAX_STACK_DEPTH; i++) {
        slot->frames[i] = sample->frames[i];
        slot->instr_ptrs[i] = sample->instr_ptrs[i];
    }

    /* Copy native PC addresses */
    for (int i = 0; i < sample->native_depth && i < SPPROF_MAX_STACK_DEPTH; i++) {
        slot->native_pcs[i] = sample->native_pcs[i];
    }

    /*
     * Publish: make the sample visible to consumer.
     *
     * Memory ordering note: memory_order_release guarantees that ALL preceding
     * writes (including the non-atomic slot copies above) are visible before
     * this store completes. On ARM64, this compiles to a stlr (store-release)
     * instruction which provides the necessary hardware barrier. An explicit
     * dmb instruction is NOT needed - C11 atomics handle this portably.
     *
     * The consumer's ATOMIC_LOAD_ACQUIRE on write_idx pairs with this release
     * store to form a proper synchronizes-with relationship.
     */
    ATOMIC_STORE_RELEASE(&rb->write_idx, next_pos);

    return 1;
}

int ringbuffer_read(RingBuffer* rb, RawSample* out) {
    /* Read current read position */
    uint64_t read_pos = ATOMIC_LOAD_RELAXED(&rb->read_idx);

    /*
     * Check if buffer is empty.
     *
     * Memory ordering: ACQUIRE pairs with the producer's RELEASE store.
     * This ensures all slot data written by the producer is visible to us
     * before we read write_idx > read_pos.
     */
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
    out->native_depth = slot->native_depth;

    /* Copy Python frames */
    for (int i = 0; i < slot->depth && i < SPPROF_MAX_STACK_DEPTH; i++) {
        out->frames[i] = slot->frames[i];
        out->instr_ptrs[i] = slot->instr_ptrs[i];
    }

    /* Copy native PCs */
    for (int i = 0; i < slot->native_depth && i < SPPROF_MAX_STACK_DEPTH; i++) {
        out->native_pcs[i] = slot->native_pcs[i];
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


