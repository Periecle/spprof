/* SPDX-License-Identifier: MIT
 * linux_memprof.c - Linux LD_PRELOAD interposition
 *
 * This file provides malloc/free interposition via LD_PRELOAD.
 * It resolves real allocator functions via dlsym(RTLD_NEXT, ...).
 *
 * CRITICAL: This file is compiled as part of the main extension for
 * integration purposes. For standalone LD_PRELOAD usage, a separate
 * shared library (libspprof_alloc.so) would be built.
 */

#if defined(__linux__)

#define _GNU_SOURCE
#include "../memprof/memprof.h"
#include "../memprof/sampling.h"
#include <dlfcn.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

/* ============================================================================
 * Real Allocator Function Pointers
 * ============================================================================ */

static void* (*real_malloc)(size_t) = NULL;
static void* (*real_calloc)(size_t, size_t) = NULL;
static void* (*real_realloc)(void*, size_t) = NULL;
static void  (*real_free)(void*) = NULL;
static int   (*real_posix_memalign)(void**, size_t, size_t) = NULL;
static void* (*real_aligned_alloc)(size_t, size_t) = NULL;
static void* (*real_memalign)(size_t, size_t) = NULL;

/* ============================================================================
 * Bootstrap Heap (for dlsym recursion)
 * ============================================================================ */

/*
 * CRITICAL: dlsym RECURSION TRAP
 *
 * On some platforms (Alpine/musl, certain glibc versions), dlsym() itself
 * calls malloc or calloc internally. This creates infinite recursion:
 *   malloc() -> ensure_initialized() -> dlsym() -> calloc() -> ... -> BOOM
 *
 * Solution: Bootstrap heap + initialization guard
 */
#define BOOTSTRAP_HEAP_SIZE (64 * 1024)  /* 64KB */
static char bootstrap_heap[BOOTSTRAP_HEAP_SIZE] __attribute__((aligned(16)));
static _Atomic size_t bootstrap_offset = 0;
static _Atomic int initializing = 0;
static _Atomic int initialized = 0;

static void* bootstrap_malloc(size_t size) {
    /* Align to 16 bytes */
    size = (size + 15) & ~(size_t)15;
    size_t offset = atomic_fetch_add(&bootstrap_offset, size);
    if (offset + size > BOOTSTRAP_HEAP_SIZE) {
        /* Bootstrap heap exhausted */
        return NULL;
    }
    return &bootstrap_heap[offset];
}

static void* bootstrap_calloc(size_t n, size_t size) {
    size_t total = n * size;
    void* p = bootstrap_malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

static int is_bootstrap_ptr(void* ptr) {
    return (ptr >= (void*)bootstrap_heap &&
            ptr < (void*)(bootstrap_heap + sizeof(bootstrap_heap)));
}

/* ============================================================================
 * Initialization
 * ============================================================================ */

static void ensure_initialized(void) {
    if (LIKELY(atomic_load_explicit(&initialized, memory_order_acquire))) {
        return;
    }
    
    /* Prevent recursion: if we're already initializing, use bootstrap */
    int expected = 0;
    if (!atomic_compare_exchange_strong(&initializing, &expected, 1)) {
        return;  /* Recursive call during init - bootstrap_* will be used */
    }
    
    /* dlsym may call malloc/calloc - those calls will use bootstrap heap */
    real_malloc = dlsym(RTLD_NEXT, "malloc");
    real_calloc = dlsym(RTLD_NEXT, "calloc");
    real_realloc = dlsym(RTLD_NEXT, "realloc");
    real_free = dlsym(RTLD_NEXT, "free");
    real_posix_memalign = dlsym(RTLD_NEXT, "posix_memalign");
    real_aligned_alloc = dlsym(RTLD_NEXT, "aligned_alloc");
    real_memalign = dlsym(RTLD_NEXT, "memalign");
    
    /*
     * CRITICAL: Handle dlsym failure (static linking, musl edge cases).
     *
     * If real_malloc is NULL after dlsym, we're in an unusual environment.
     * Fail fast with a clear error message.
     */
    if (real_malloc == NULL) {
        const char msg[] =
            "[spprof] FATAL: dlsym(RTLD_NEXT, \"malloc\") returned NULL.\n"
            "This typically means:\n"
            "  - The binary is statically linked (LD_PRELOAD won't work)\n"
            "  - The libc doesn't support RTLD_NEXT properly\n"
            "\n"
            "The memory profiler REQUIRES dynamic linking. Aborting.\n";
        ssize_t r = write(STDERR_FILENO, msg, sizeof(msg) - 1);
        (void)r;  /* Suppress unused result warning */
        _exit(1);
    }
    
    atomic_store_explicit(&initialized, 1, memory_order_release);
    atomic_store_explicit(&initializing, 0, memory_order_relaxed);
}

/* ============================================================================
 * Allocation Hooks (Internal - called by sampling engine)
 * ============================================================================ */

/* These are NOT the LD_PRELOAD entry points - those would be in a separate
 * shared library. These are internal functions for when the profiler is
 * loaded as a Python extension and wants to hook allocations.
 *
 * For now, on Linux we rely on the Python extension being able to hook
 * PyMem allocators, or we provide a separate LD_PRELOAD library.
 */

static void handle_malloc(void* ptr, size_t size) {
    if (!ptr || !atomic_load_explicit(&g_memprof.active_alloc, memory_order_relaxed)) {
        return;
    }
    
    /* Check fork safety */
    if (UNLIKELY(sampling_in_forked_child())) {
        atomic_store_explicit(&g_memprof.active_alloc, 0, memory_order_relaxed);
        atomic_store_explicit(&g_memprof.active_free, 0, memory_order_relaxed);
        return;
    }
    
    MemProfThreadState* tls = sampling_get_tls();
    if (!tls->initialized) {
        sampling_ensure_tls_init();
        tls = sampling_get_tls();
    }
    
    if (tls->inside_profiler) {
        tls->skipped_reentrant++;
        return;
    }
    
    tls->total_allocs++;
    
    if (sampling_should_sample(tls, size)) {
        tls->inside_profiler = 1;
        sampling_handle_sample(ptr, size);
        tls->inside_profiler = 0;
    }
}

static void handle_free(void* ptr) {
    if (!ptr || !atomic_load_explicit(&g_memprof.active_free, memory_order_relaxed)) {
        return;
    }
    
    MemProfThreadState* tls = sampling_get_tls();
    if (!tls->initialized) {
        sampling_ensure_tls_init();
        tls = sampling_get_tls();
    }
    
    if (tls->inside_profiler) {
        return;
    }
    
    tls->total_frees++;
    
    tls->inside_profiler = 1;
    sampling_handle_free(ptr);
    tls->inside_profiler = 0;
}

/* ============================================================================
 * Installation / Removal (Python Extension Mode)
 * ============================================================================ */

/*
 * On Linux, when loaded as a Python extension, we can't easily intercept
 * all malloc calls. We have two options:
 *
 * 1. Use PyMem_SetAllocator to hook Python allocations only
 * 2. Require LD_PRELOAD for full native allocation tracking
 *
 * For now, we provide stub functions that the Python extension can call.
 * Full native tracking requires the separate libspprof_alloc.so library.
 */

static int g_linux_hooks_installed = 0;

int memprof_linux_install(void) {
    ensure_initialized();
    
    if (g_linux_hooks_installed) {
        return -1;  /* Already installed */
    }
    
    g_linux_hooks_installed = 1;
    
    /* TODO: Optionally install PyMem hooks here for Python-only tracking */
    
    return 0;
}

void memprof_linux_remove(void) {
    g_linux_hooks_installed = 0;
    
    /* TODO: Remove PyMem hooks if installed */
}

/* ============================================================================
 * LD_PRELOAD Entry Points (for libspprof_alloc.so)
 *
 * These functions would be the actual LD_PRELOAD hooks when building
 * the standalone shared library. They're included here for reference
 * but guarded by SPPROF_BUILD_PRELOAD.
 * ============================================================================ */

#ifdef SPPROF_BUILD_PRELOAD

void* malloc(size_t size) {
    if (UNLIKELY(atomic_load_explicit(&initializing, memory_order_relaxed))) {
        return bootstrap_malloc(size);
    }
    
    ensure_initialized();
    
    void* ptr = real_malloc(size);
    handle_malloc(ptr, size);
    return ptr;
}

void* calloc(size_t n, size_t size) {
    if (UNLIKELY(atomic_load_explicit(&initializing, memory_order_relaxed))) {
        return bootstrap_calloc(n, size);
    }
    
    ensure_initialized();
    
    void* ptr = real_calloc(n, size);
    handle_malloc(ptr, n * size);
    return ptr;
}

void* realloc(void* ptr, size_t size) {
    if (UNLIKELY(is_bootstrap_ptr(ptr))) {
        /* Can't realloc bootstrap memory - allocate new and copy */
        void* new_ptr = bootstrap_malloc(size);
        if (new_ptr && ptr) {
            memcpy(new_ptr, ptr, size);  /* May copy garbage, but safe */
        }
        return new_ptr;
    }
    
    ensure_initialized();
    
    /* Handle free of old ptr */
    if (ptr) {
        handle_free(ptr);
    }
    
    void* new_ptr = real_realloc(ptr, size);
    
    /* Handle malloc of new ptr */
    if (new_ptr) {
        handle_malloc(new_ptr, size);
    }
    
    return new_ptr;
}

void free(void* ptr) {
    if (!ptr) return;
    
    /* Bootstrap allocations cannot be freed */
    if (UNLIKELY(is_bootstrap_ptr(ptr))) {
        return;
    }
    
    ensure_initialized();
    
    handle_free(ptr);
    real_free(ptr);
}

int posix_memalign(void** memptr, size_t alignment, size_t size) {
    ensure_initialized();
    
    int result = real_posix_memalign(memptr, alignment, size);
    if (result == 0 && *memptr) {
        handle_malloc(*memptr, size);
    }
    return result;
}

void* aligned_alloc(size_t alignment, size_t size) {
    ensure_initialized();
    
    void* ptr = real_aligned_alloc(alignment, size);
    handle_malloc(ptr, size);
    return ptr;
}

void* memalign(size_t alignment, size_t size) {
    ensure_initialized();
    
    void* ptr = real_memalign(alignment, size);
    handle_malloc(ptr, size);
    return ptr;
}

#endif /* SPPROF_BUILD_PRELOAD */

#endif /* __linux__ */

