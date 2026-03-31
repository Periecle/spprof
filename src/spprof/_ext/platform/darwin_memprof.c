/* SPDX-License-Identifier: MIT
 * darwin_memprof.c - macOS malloc_logger interposition
 *
 * Uses Apple's official malloc_logger callback mechanism to intercept
 * all memory allocations across all zones.
 */

#if defined(__APPLE__)

#include "../memprof/memprof.h"
#include "../memprof/sampling.h"
#include <malloc/malloc.h>
#include <stdatomic.h>
#include <time.h>
#include <errno.h>

/* ============================================================================
 * malloc_logger Callback
 * ============================================================================ */

/* Apple's callback type */
typedef void (*malloc_logger_t)(uint32_t type, uintptr_t arg1,
                                uintptr_t arg2, uintptr_t arg3,
                                uintptr_t result, uint32_t num_hot_frames);

extern malloc_logger_t malloc_logger;

/* Atomic flag for thread-safe installation */
static _Atomic(malloc_logger_t) g_installed_logger = NULL;

/* Thread-local re-entrancy guard using pthread_key for reliability on macOS.
 * __thread can be problematic with dynamic libraries on Apple Silicon. */
#include <pthread.h>
static pthread_key_t g_in_logger_key;
static _Atomic int g_key_initialized = 0;
static _Atomic int g_key_init_failed = 0;

static void ensure_key_initialized(void) {
    /* Fast path: already initialized */
    if (atomic_load_explicit(&g_key_initialized, memory_order_acquire)) {
        return;
    }
    
    /* Check if previous initialization failed */
    if (atomic_load_explicit(&g_key_init_failed, memory_order_acquire)) {
        return;
    }
    
    int expected = 0;
    if (atomic_compare_exchange_strong_explicit(&g_key_initialized, &expected, -1,
                                                 memory_order_acq_rel, memory_order_relaxed)) {
        /* We won the race - initialize the key */
        int result = pthread_key_create(&g_in_logger_key, NULL);
        if (result != 0) {
            atomic_store_explicit(&g_key_init_failed, 1, memory_order_release);
            atomic_store_explicit(&g_key_initialized, 0, memory_order_release);
            return;
        }
        atomic_store_explicit(&g_key_initialized, 1, memory_order_release);
    } else {
        /* Another thread is initializing - spin wait */
        while (atomic_load_explicit(&g_key_initialized, memory_order_acquire) == -1) {
            /* Brief spin */
        }
    }
}

static int get_in_logger(void) {
    if (!atomic_load_explicit(&g_key_initialized, memory_order_acquire) ||
        atomic_load_explicit(&g_key_init_failed, memory_order_acquire)) {
        return 1;  /* Fail safe: pretend we're in logger to skip profiling */
    }
    return (int)(intptr_t)pthread_getspecific(g_in_logger_key);
}

static void set_in_logger(int val) {
    if (!atomic_load_explicit(&g_key_initialized, memory_order_acquire) ||
        atomic_load_explicit(&g_key_init_failed, memory_order_acquire)) {
        return;  /* Key not available */
    }
    pthread_setspecific(g_in_logger_key, (void*)(intptr_t)val);
}

/*
 * Type bits (empirically determined on macOS 15):
 *   0x02 = allocation (malloc, calloc, realloc result) - NEW allocation
 *   0x04 = deallocation (free, realloc source) - FREE operation
 *   0x08 = always set (unknown purpose)
 *   0x40 = cleared memory (calloc)
 *
 * Examples:
 *   malloc:  0x0a = 0000 1010 (alloc + bit3)
 *   free:    0x0c = 0000 1100 (free + bit3)
 *   calloc:  0x4a = 0100 1010 (alloc + bit3 + cleared)
 *   realloc: 0x0e = 0000 1110 (alloc + free + bit3)
 *
 * For allocations: arg2 = size, result = pointer
 * For frees: arg2 = pointer being freed
 */
static void spprof_malloc_logger(uint32_t type, uintptr_t arg1,
                                  uintptr_t arg2, uintptr_t arg3,
                                  uintptr_t result, uint32_t num_hot_frames) {
    (void)arg1; (void)arg3; (void)num_hot_frames;
    
    /* Ensure pthread key is ready before any TLS access */
    ensure_key_initialized();
    
    /* CRITICAL: Early re-entrancy check using pthread TLS.
     * This prevents infinite recursion when sampling_ensure_tls_init() 
     * calls functions that allocate. */
    if (get_in_logger()) {
        return;
    }
    set_in_logger(1);
    
    /* Early exit if being uninstalled (prevents use-after-free during removal) */
    if (atomic_load_explicit(&g_installed_logger, memory_order_acquire) == NULL) {
        set_in_logger(0);
        return;
    }
    
    /* Check if we're in a forked child - disable profiler */
    if (UNLIKELY(sampling_in_forked_child())) {
        atomic_store_explicit(&g_memprof.active_alloc, 0, memory_order_relaxed);
        atomic_store_explicit(&g_memprof.active_free, 0, memory_order_relaxed);
        set_in_logger(0);
        return;
    }
    
    /* Get TLS and check re-entrancy */
    MemProfThreadState* tls = sampling_get_tls();
    if (!tls->initialized) {
        sampling_ensure_tls_init();
        tls = sampling_get_tls();
    }
    
    if (tls->inside_profiler) {
        tls->skipped_reentrant++;
        set_in_logger(0);
        return;
    }
    
    /* Handle allocations (type & 0x02) - bit 1 indicates new allocation */
    if (type & 0x02) {
        size_t size = (size_t)arg2;
        void* ptr = (void*)result;
        
        if (!ptr || !atomic_load_explicit(&g_memprof.active_alloc, memory_order_relaxed)) {
            set_in_logger(0);
            return;
        }
        
        tls->total_allocs++;
        
        /* Sampling decision */
        if (sampling_should_sample(tls, size)) {
            tls->inside_profiler = 1;
            sampling_handle_sample(ptr, size);
            tls->inside_profiler = 0;
        }
    }
    
    /* Handle deallocations (type & 0x04) - bit 2 indicates free/realloc source */
    if (type & 0x04) {
        void* ptr = (void*)arg2;
        
        if (!ptr || !atomic_load_explicit(&g_memprof.active_free, memory_order_relaxed)) {
            set_in_logger(0);
            return;
        }
        
        tls->total_frees++;
        
        tls->inside_profiler = 1;
        sampling_handle_free(ptr);
        tls->inside_profiler = 0;
    }
    
    set_in_logger(0);
}

/* ============================================================================
 * Installation / Removal
 * ============================================================================ */

int memprof_darwin_install(void) {
    /* Initialize pthread key BEFORE installing callback to avoid recursion */
    ensure_key_initialized();
    
    /* Check if already installed - make this idempotent */
    malloc_logger_t current = atomic_load_explicit(&g_installed_logger, memory_order_acquire);
    if (current == spprof_malloc_logger) {
        /* Already installed - ensure callback is set and return success */
        malloc_logger = spprof_malloc_logger;
        return 0;
    }
    
    /* Try to install */
    malloc_logger_t expected = NULL;
    if (!atomic_compare_exchange_strong_explicit(&g_installed_logger,
                                                  &expected,
                                                  spprof_malloc_logger,
                                                  memory_order_acq_rel,
                                                  memory_order_relaxed)) {
        /* Someone else installed (could be us) - check if it's our callback */
        if (expected == spprof_malloc_logger) {
            return 0;  /* Already installed by us */
        }
        return -1;  /* Different callback installed */
    }
    
    /* Memory fence ensures g_installed_logger is visible before callback */
    atomic_thread_fence(memory_order_seq_cst);
    malloc_logger = spprof_malloc_logger;
    
    return 0;
}

void memprof_darwin_remove(void) {
    /* Mark as uninstalling first */
    atomic_store_explicit(&g_installed_logger, NULL, memory_order_release);
    atomic_thread_fence(memory_order_seq_cst);
    
    /* Clear the callback */
    malloc_logger = NULL;
    
    /* Brief delay to let in-flight callbacks complete.
     * Callbacks check g_installed_logger and exit early if NULL.
     * Use nanosleep instead of usleep for POSIX.1-2001 compliance.
     * 
     * We use a slightly longer delay (5ms) to be safe across all cores. */
    struct timespec ts = {0, 5000000};  /* 5ms */
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
        /* Retry if interrupted by signal */
    }
}

#endif /* __APPLE__ */

