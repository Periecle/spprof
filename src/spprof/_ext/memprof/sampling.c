/* SPDX-License-Identifier: MIT
 * sampling.c - Poisson sampling engine
 *
 * Implements Poisson sampling with exponential inter-sample intervals.
 */

#include "sampling.h"
#include "heap_map.h"
#include "stack_intern.h"
#include "bloom.h"
#include "stack_capture.h"
#include "memprof.h"
#include <math.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#endif

/* ============================================================================
 * Thread-Local Storage
 * ============================================================================ */

#ifdef _WIN32
static __declspec(thread) MemProfThreadState tls_state = {0};
#else
static __thread MemProfThreadState tls_state = {0};
#endif

/* Global seed entropy - read once from system */
static uint64_t g_global_seed = 0;
static _Atomic int g_seed_initialized = 0;

/* Process ID at init time (for fork detection) */
static pid_t g_init_pid = 0;

/* ============================================================================
 * Global Seed Initialization
 * ============================================================================ */

static void init_global_seed_once(void) {
    int expected = 0;
    if (!atomic_compare_exchange_strong(&g_seed_initialized, &expected, 1)) {
        return;  /* Already done */
    }
    
#ifdef _WIN32
    /* Windows: Use CryptGenRandom or QueryPerformanceCounter */
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    g_global_seed = (uint64_t)counter.QuadPart ^ (uint64_t)GetCurrentProcessId();
#else
    /* Use a simple but allocation-free entropy source.
     * NOTE: We avoid open("/dev/urandom") as it can trigger allocations
     * on some systems, causing infinite recursion in malloc_logger. */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    g_global_seed = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    g_global_seed ^= (uint64_t)getpid() << 32;
    g_global_seed *= 0x5851F42D4C957F2DULL;
#endif
}

/* ============================================================================
 * PRNG (xorshift128+)
 * ============================================================================ */

uint64_t prng_next(uint64_t state[2]) {
    uint64_t s0 = state[0];
    uint64_t s1 = state[1];
    uint64_t result = s0 + s1;
    
    s1 ^= s0;
    state[0] = ((s0 << 55) | (s0 >> 9)) ^ s1 ^ (s1 << 14);
    state[1] = (s1 << 36) | (s1 >> 28);
    
    return result;
}

double prng_next_double(uint64_t state[2]) {
    return (double)(prng_next(state) >> 11) * (1.0 / (double)(1ULL << 53));
}

/* ============================================================================
 * Threshold Generation
 * ============================================================================ */

int64_t next_sample_threshold(uint64_t state[2], uint64_t mean_bytes) {
    if (!state || mean_bytes == 0) {
        return MEMPROF_DEFAULT_SAMPLING_RATE;
    }
    
    double u = prng_next_double(state);
    
    /* Clamp to prevent ln(0) and extreme values.
     * u = 1e-10 → threshold ≈ 23×mean (reasonable upper bound) */
    if (u < 1e-10) u = 1e-10;
    if (u > 1.0 - 1e-10) u = 1.0 - 1e-10;
    
    double threshold = -((double)mean_bytes) * log(u);
    
    /* Clamp to reasonable range: [1 byte, 1TB] */
    if (threshold < 1.0) threshold = 1.0;
    if (threshold > (double)(1ULL << 40)) threshold = (double)(1ULL << 40);
    
    return (int64_t)threshold;
}

/* ============================================================================
 * TLS Management
 * ============================================================================ */

MemProfThreadState* sampling_get_tls(void) {
    return &tls_state;
}

void sampling_ensure_tls_init(void) {
    if (LIKELY(tls_state.initialized)) {
        return;
    }
    
    init_global_seed_once();
    
    /* Seed PRNG with thread-unique + process-unique + global entropy */
    uint64_t tid = (uint64_t)(uintptr_t)&tls_state;
    uint64_t time_ns = memprof_get_monotonic_ns();
    uint64_t pid = (uint64_t)getpid();
    
    tls_state.prng_state[0] = tid ^ time_ns ^ g_global_seed ^ 0x123456789ABCDEF0ULL;
    tls_state.prng_state[1] = (tid << 32) ^ (time_ns >> 32) ^ (pid << 48) ^
                              g_global_seed ^ 0xFEDCBA9876543210ULL;
    
    /* Mix state to avoid correlated initial sequences */
    for (int i = 0; i < 10; i++) {
        (void)prng_next(tls_state.prng_state);
    }
    
    /* Set initial sampling threshold */
    uint64_t rate = g_memprof.sampling_rate;
    if (rate == 0) rate = MEMPROF_DEFAULT_SAMPLING_RATE;
    tls_state.byte_counter = next_sample_threshold(tls_state.prng_state, rate);
    
    tls_state.inside_profiler = 0;
    tls_state.frame_depth = 0;
    tls_state.total_allocs = 0;
    tls_state.total_frees = 0;
    tls_state.sampled_allocs = 0;
    tls_state.sampled_bytes = 0;
    tls_state.skipped_reentrant = 0;
    
    tls_state.initialized = 1;
}

void sampling_reset_threshold(MemProfThreadState* tls) {
    uint64_t rate = g_memprof.sampling_rate;
    if (rate == 0) rate = MEMPROF_DEFAULT_SAMPLING_RATE;
    tls->byte_counter = next_sample_threshold(tls->prng_state, rate);
}

/* ============================================================================
 * Cold Path: Handle Sampled Allocation
 * ============================================================================ */

void sampling_handle_sample(void* ptr, size_t size) {
    if (!ptr || !atomic_load_explicit(&g_memprof.active_alloc, memory_order_relaxed)) {
        return;
    }
    
    MemProfThreadState* tls = sampling_get_tls();
    if (!tls) {
        return;
    }
    
    /* Re-entrancy guard - must be set by caller! */
    if (UNLIKELY(!tls->inside_profiler)) {
        return;
    }
    
    /* Update stats */
    tls->sampled_allocs++;
    tls->sampled_bytes += size;
    
    /* Get global sequence number for ABA detection */
    uint64_t birth_seq = atomic_fetch_add_explicit(&g_memprof.global_seq, 1,
                                                    memory_order_relaxed);
    uint64_t timestamp = memprof_get_monotonic_ns();
    
    /* Phase 1: Reserve heap map slot */
    int slot_idx = heap_map_reserve((uintptr_t)ptr);
    if (slot_idx < 0) {
        /* Table full - graceful degradation */
        sampling_reset_threshold(tls);
        return;
    }
    
    /* Capture stack trace */
    MixedStackCapture capture = {0};
    int total_frames = capture_mixed_stack(&capture);
    
    /* Check frame pointer health */
    check_frame_pointer_health(capture.native_depth, capture.python_depth);
    
    /* Intern the stack (with both native and Python frames) */
    uint32_t stack_id = UINT32_MAX;
    if (total_frames > 0 && capture.native_depth > 0) {
        stack_id = stack_table_intern(
            capture.native_pcs, capture.native_depth,
            capture.python_code_ptrs, capture.python_depth);
    }
    
    /* Calculate weight (= sampling rate) */
    uint32_t weight = (uint32_t)g_memprof.sampling_rate;
    if (weight == 0) weight = MEMPROF_DEFAULT_SAMPLING_RATE;
    
    /* Clamp size */
    uint32_t size32 = (size > MAX_ALLOC_SIZE) ? MAX_ALLOC_SIZE : (uint32_t)size;
    
    /* Phase 2: Finalize heap map entry */
    int success = heap_map_finalize(slot_idx, (uintptr_t)ptr, stack_id,
                                     size32, weight, birth_seq, timestamp);
    
    if (success) {
        /* Add to Bloom filter */
        bloom_add((uintptr_t)ptr);
        atomic_fetch_add_explicit(&g_memprof.total_samples, 1, memory_order_relaxed);
    }
    
    /* Check if Bloom filter needs rebuilding (infrequent check) */
    static _Atomic uint32_t rebuild_check_counter = 0;
    uint32_t check = atomic_fetch_add_explicit(&rebuild_check_counter, 1, memory_order_relaxed);
    if ((check & 0xFF) == 0 && bloom_needs_rebuild() && 
        !atomic_load_explicit(&g_memprof.bloom_rebuild_in_progress, memory_order_relaxed)) {
        /* Trigger rebuild (non-blocking, will be handled asynchronously or skipped) */
        bloom_rebuild_from_heap();
    }
    
    /* Reset threshold */
    sampling_reset_threshold(tls);
}

/* ============================================================================
 * Handle Free
 * ============================================================================ */

void sampling_handle_free(void* ptr) {
    if (!ptr || !atomic_load_explicit(&g_memprof.active_free, memory_order_relaxed)) {
        return;
    }
    
    /* Fast path: Bloom filter check */
    if (!bloom_might_contain((uintptr_t)ptr)) {
        return;  /* Definitely not sampled */
    }
    
    /* Get sequence number for ABA detection BEFORE looking up */
    uint64_t free_seq = atomic_fetch_add_explicit(&g_memprof.global_seq, 1,
                                                   memory_order_relaxed);
    uint64_t free_timestamp = memprof_get_monotonic_ns();
    
    /* Look up and remove from heap map */
    uint32_t stack_id, size, weight;
    uint64_t duration;
    
    heap_map_remove((uintptr_t)ptr, free_seq, free_timestamp,
                    &stack_id, &size, &weight, &duration);
}

/* ============================================================================
 * Fork Safety
 * ============================================================================ */

#ifndef _WIN32

static void memprof_prefork(void) {
    /* Acquire any "soft locks" (atomic flags used as locks) */
    while (atomic_exchange_explicit(&g_memprof.bloom_rebuild_in_progress, 1,
                                     memory_order_acquire)) {
        /* Spin until we own it - brief, fork is rare */
        struct timespec ts = {0, 1000};  /* 1µs */
        nanosleep(&ts, NULL);
    }
}

static void memprof_postfork_parent(void) {
    /* Release lock in parent */
    atomic_store_explicit(&g_memprof.bloom_rebuild_in_progress, 0, memory_order_release);
}

static void memprof_postfork_child(void) {
    /* In child: Reset all state, profiler is effectively disabled
     * until explicitly restarted. */
    atomic_store_explicit(&g_memprof.bloom_rebuild_in_progress, 0, memory_order_relaxed);
    atomic_store_explicit(&g_memprof.active_alloc, 0, memory_order_relaxed);
    atomic_store_explicit(&g_memprof.active_free, 0, memory_order_relaxed);
    
    /* TLS is per-thread, child's main thread gets fresh TLS on first use */
    tls_state.initialized = 0;
}

int sampling_register_fork_handlers(void) {
    return pthread_atfork(memprof_prefork,
                          memprof_postfork_parent,
                          memprof_postfork_child);
}

#else  /* _WIN32 */

int sampling_register_fork_handlers(void) {
    return 0;  /* Windows doesn't have fork() */
}

#endif

int sampling_in_forked_child(void) {
    if (UNLIKELY(g_init_pid == 0)) {
        g_init_pid = getpid();
        return 0;
    }
    return getpid() != g_init_pid;
}

