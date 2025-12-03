/* SPDX-License-Identifier: MIT
 * bloom.c - Bloom filter for free() hot path optimization
 *
 * 99.99% of frees are for non-sampled allocations. The Bloom filter
 * provides O(1) definite-no answers with 0% false negatives.
 */

#include "bloom.h"
#include "heap_map.h"
#include "memprof.h"
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

/* ============================================================================
 * Leaked Filter Tracking
 * ============================================================================ */

typedef struct LeakedFilter {
    _Atomic uint8_t* filter;
    struct LeakedFilter* next;
} LeakedFilter;

static _Atomic(LeakedFilter*) g_leaked_filters = NULL;

/* Maximum number of leaked filters to track (prevents unbounded growth) */
#define MAX_LEAKED_FILTERS 16
static _Atomic uint32_t g_leaked_filter_count = 0;

static void record_leaked_filter(_Atomic uint8_t* filter) {
    if (!filter) return;
    
    /* Limit tracked filters to prevent memory growth */
    uint32_t count = atomic_fetch_add_explicit(&g_leaked_filter_count, 1, memory_order_relaxed);
    if (count >= MAX_LEAKED_FILTERS) {
        /* Too many - just free it directly (caller must ensure safe) */
        atomic_fetch_sub_explicit(&g_leaked_filter_count, 1, memory_order_relaxed);
#ifdef _WIN32
        VirtualFree((void*)filter, 0, MEM_RELEASE);
#else
        munmap((void*)filter, BLOOM_SIZE_BYTES);
#endif
        return;
    }
    
    /* Allocate tracking node via mmap (can't use malloc in profiler code) */
#ifdef _WIN32
    LeakedFilter* node = (LeakedFilter*)VirtualAlloc(
        NULL, sizeof(LeakedFilter),
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!node) {
        atomic_fetch_sub_explicit(&g_leaked_filter_count, 1, memory_order_relaxed);
        VirtualFree((void*)filter, 0, MEM_RELEASE);
        return;
    }
#else
    LeakedFilter* node = (LeakedFilter*)mmap(
        NULL, sizeof(LeakedFilter),
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (node == MAP_FAILED) {
        atomic_fetch_sub_explicit(&g_leaked_filter_count, 1, memory_order_relaxed);
        munmap((void*)filter, BLOOM_SIZE_BYTES);
        return;
    }
#endif
    
    node->filter = filter;
    
    /* Lock-free push to front of list */
    LeakedFilter* old_head;
    do {
        old_head = atomic_load_explicit(&g_leaked_filters, memory_order_relaxed);
        node->next = old_head;
    } while (!atomic_compare_exchange_weak_explicit(
        &g_leaked_filters, &old_head, node,
        memory_order_release, memory_order_relaxed));
}

/* ============================================================================
 * Hash Functions
 * ============================================================================ */

void bloom_get_indices(uintptr_t ptr, uint64_t indices[BLOOM_HASH_COUNT]) {
    /* Double-hashing scheme: h(i) = h1 + i*h2 */
    uint64_t h1 = (uint64_t)ptr * 0x9E3779B97F4A7C15ULL;  /* Golden ratio */
    uint64_t h2 = (uint64_t)ptr * 0xC96C5795D7870F42ULL;  /* Another prime */
    
    for (int i = 0; i < BLOOM_HASH_COUNT; i++) {
        indices[i] = (h1 + (uint64_t)i * h2) & (BLOOM_SIZE_BITS - 1);
    }
}

/* ============================================================================
 * Initialization
 * ============================================================================ */

int bloom_init(void) {
    _Atomic uint8_t* filter;
    
#ifdef _WIN32
    filter = (_Atomic uint8_t*)VirtualAlloc(
        NULL, BLOOM_SIZE_BYTES,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!filter) {
        return -1;
    }
#else
    filter = (_Atomic uint8_t*)mmap(
        NULL, BLOOM_SIZE_BYTES,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (filter == MAP_FAILED) {
        return -1;
    }
#endif
    
    /* mmap returns zero-initialized memory */
    memset((void*)filter, 0, BLOOM_SIZE_BYTES);
    
    atomic_store_explicit(&g_memprof.bloom_filter_ptr, filter, memory_order_release);
    atomic_store_explicit(&g_memprof.bloom_ones_count, 0, memory_order_relaxed);
    atomic_store_explicit(&g_memprof.bloom_rebuild_in_progress, 0, memory_order_relaxed);
    
    return 0;
}

/* ============================================================================
 * Add Operation
 * ============================================================================ */

void bloom_add(uintptr_t ptr) {
    _Atomic uint8_t* filter = atomic_load_explicit(&g_memprof.bloom_filter_ptr,
                                                    memory_order_acquire);
    if (!filter) return;
    
    uint64_t indices[BLOOM_HASH_COUNT];
    bloom_get_indices(ptr, indices);
    
    for (int i = 0; i < BLOOM_HASH_COUNT; i++) {
        uint64_t byte_idx = indices[i] / 8;
        uint8_t bit_mask = (uint8_t)(1 << (indices[i] % 8));
        
        /* Atomic OR - thread safe */
        uint8_t old_val = atomic_fetch_or_explicit(&filter[byte_idx], bit_mask,
                                                    memory_order_relaxed);
        
        /* Track new bits set (approximate - may double-count under contention) */
        if (!(old_val & bit_mask)) {
            atomic_fetch_add_explicit(&g_memprof.bloom_ones_count, 1,
                                      memory_order_relaxed);
        }
    }
}

/* ============================================================================
 * Query Operation
 * ============================================================================ */

int bloom_might_contain(uintptr_t ptr) {
    _Atomic uint8_t* filter = atomic_load_explicit(&g_memprof.bloom_filter_ptr,
                                                    memory_order_acquire);
    if (!filter) return 0;
    
    uint64_t indices[BLOOM_HASH_COUNT];
    bloom_get_indices(ptr, indices);
    
    for (int i = 0; i < BLOOM_HASH_COUNT; i++) {
        uint64_t byte_idx = indices[i] / 8;
        uint8_t bit_mask = (uint8_t)(1 << (indices[i] % 8));
        uint8_t byte_val = atomic_load_explicit(&filter[byte_idx],
                                                 memory_order_relaxed);
        
        if (!(byte_val & bit_mask)) {
            return 0;  /* Definitely NOT in set */
        }
    }
    
    return 1;  /* Maybe in set - check heap map */
}

/* ============================================================================
 * Saturation Monitoring
 * ============================================================================ */

int bloom_needs_rebuild(void) {
    uint64_t ones = atomic_load_explicit(&g_memprof.bloom_ones_count,
                                          memory_order_relaxed);
    return ones > (BLOOM_SIZE_BITS / 2);
}

int bloom_saturation_percent(void) {
    uint64_t ones = atomic_load_explicit(&g_memprof.bloom_ones_count,
                                          memory_order_relaxed);
    return (int)((ones * 100) / BLOOM_SIZE_BITS);
}

/* ============================================================================
 * Rebuild from Heap Map
 * ============================================================================ */

/* Callback for heap map iteration */
static void add_to_new_filter_cb(const HeapMapEntry* entry, void* user_data) {
    /* user_data contains [filter_ptr, ones_count_ptr] */
    void** ptrs = (void**)user_data;
    uint8_t* new_filter = (uint8_t*)ptrs[0];
    uint64_t* new_ones = (uint64_t*)ptrs[1];
    
    uintptr_t ptr = atomic_load_explicit(&entry->ptr, memory_order_relaxed);
    
    uint64_t indices[BLOOM_HASH_COUNT];
    bloom_get_indices(ptr, indices);
    
    for (int j = 0; j < BLOOM_HASH_COUNT; j++) {
        uint64_t byte_idx = indices[j] / 8;
        uint8_t bit_mask = (uint8_t)(1 << (indices[j] % 8));
        
        /* Non-atomic access OK - new filter not published yet */
        if (!(new_filter[byte_idx] & bit_mask)) {
            new_filter[byte_idx] |= bit_mask;
            (*new_ones)++;
        }
    }
}

int bloom_rebuild_from_heap(void) {
    /* Try to acquire rebuild lock */
    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(
            &g_memprof.bloom_rebuild_in_progress, &expected, 1,
            memory_order_acq_rel, memory_order_relaxed)) {
        return -1;  /* Another rebuild in progress */
    }
    
    /* Allocate new filter */
    _Atomic uint8_t* new_filter;
    
#ifdef _WIN32
    new_filter = (_Atomic uint8_t*)VirtualAlloc(
        NULL, BLOOM_SIZE_BYTES,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!new_filter) {
        atomic_store_explicit(&g_memprof.bloom_rebuild_in_progress, 0,
                              memory_order_release);
        return -1;
    }
#else
    new_filter = (_Atomic uint8_t*)mmap(
        NULL, BLOOM_SIZE_BYTES,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (new_filter == MAP_FAILED) {
        atomic_store_explicit(&g_memprof.bloom_rebuild_in_progress, 0,
                              memory_order_release);
        return -1;
    }
#endif
    
    memset((void*)new_filter, 0, BLOOM_SIZE_BYTES);
    
    /* Iterate heap map, add live entries to new filter */
    uint64_t new_ones = 0;
    void* cb_data[2] = { (void*)new_filter, &new_ones };
    heap_map_iterate(add_to_new_filter_cb, cb_data);
    
    /* Atomic swap - readers see either old or new, both valid */
    _Atomic uint8_t* old_filter = atomic_load_explicit(&g_memprof.bloom_filter_ptr,
                                                        memory_order_relaxed);
    atomic_store_explicit(&g_memprof.bloom_filter_ptr, new_filter, memory_order_release);
    atomic_store_explicit(&g_memprof.bloom_ones_count, new_ones, memory_order_relaxed);
    
    /* INTENTIONALLY LEAK old_filter - record for cleanup at shutdown */
    if (old_filter) {
        record_leaked_filter(old_filter);
    }
    
    atomic_fetch_add_explicit(&g_memprof.bloom_rebuilds, 1, memory_order_relaxed);
    atomic_store_explicit(&g_memprof.bloom_rebuild_in_progress, 0, memory_order_release);
    
    return 0;
}

/* ============================================================================
 * Cleanup
 * ============================================================================ */

void bloom_cleanup_leaked_filters(void) {
    /* Atomically swap out the list head to prevent concurrent access */
    LeakedFilter* node = atomic_exchange_explicit(&g_leaked_filters, NULL, memory_order_acquire);
    
    /* Walk the leaked filter list and free them all */
    while (node) {
        LeakedFilter* next = node->next;
        
        if (node->filter) {
#ifdef _WIN32
            VirtualFree((void*)node->filter, 0, MEM_RELEASE);
#else
            munmap((void*)node->filter, BLOOM_SIZE_BYTES);
#endif
        }
        
#ifdef _WIN32
        VirtualFree(node, 0, MEM_RELEASE);
#else
        munmap(node, sizeof(LeakedFilter));
#endif
        
        node = next;
    }
    
    /* Reset counter */
    atomic_store_explicit(&g_leaked_filter_count, 0, memory_order_release);
}

void bloom_destroy(void) {
    /* Clean up leaked filters first */
    bloom_cleanup_leaked_filters();
    
    /* Free the current active filter */
    _Atomic uint8_t* current = atomic_load_explicit(&g_memprof.bloom_filter_ptr,
                                                     memory_order_relaxed);
    if (current) {
#ifdef _WIN32
        VirtualFree((void*)current, 0, MEM_RELEASE);
#else
        munmap((void*)current, BLOOM_SIZE_BYTES);
#endif
        atomic_store_explicit(&g_memprof.bloom_filter_ptr, NULL, memory_order_release);
    }
}

