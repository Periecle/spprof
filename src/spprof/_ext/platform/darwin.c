/**
 * platform/darwin.c - macOS platform implementation
 *
 * Uses pure Mach-based sampling via the darwin_mach module:
 *   - pthread_introspection_hook for thread discovery
 *   - thread_suspend/resume for safe thread stopping
 *   - thread_get_state for register capture
 *   - Direct PyThreadState access for Python frame capture
 *   - mach_wait_until for precise timing
 *
 * This replaces the signal-based approach (setitimer/SIGPROF) with a more
 * accurate and reliable suspend-walk-resume pattern.
 *
 * Copyright (c) 2024 spprof contributors
 * SPDX-License-Identifier: MIT
 */

#ifdef __APPLE__

#include <Python.h>
#include <pthread.h>
#include <mach/mach_time.h>
#include <string.h>

#include "platform.h"
#include "darwin_mach.h"
#include "../ringbuffer.h"

/* Ring buffer - shared with module.c */
extern RingBuffer* g_ringbuffer;

/* Global state */
static uint64_t g_interval_ns = 0;
static int g_platform_initialized = 0;
static int g_sampler_active = 0;

/* Mach timebase for high-resolution timing */
static mach_timebase_info_data_t g_timebase_info;
static int g_timebase_initialized = 0;

/*
 * =============================================================================
 * Platform Initialization
 * =============================================================================
 */

int platform_init(void) {
    if (g_platform_initialized) {
        return 0;
    }
    
    /* Initialize timebase info */
    if (!g_timebase_initialized) {
        mach_timebase_info(&g_timebase_info);
        g_timebase_initialized = 1;
    }
    
    /* Initialize Mach sampler subsystem.
     * This installs pthread introspection hooks and sets up thread registry. */
    if (mach_sampler_init() != 0) {
        return -1;
    }
    
    g_platform_initialized = 1;
    return 0;
}

void platform_cleanup(void) {
    /* Stop sampler if running */
    if (g_sampler_active) {
        platform_timer_destroy();
    }
    
    /* Cleanup Mach sampler */
    mach_sampler_cleanup();
    
    g_platform_initialized = 0;
}

/*
 * =============================================================================
 * Timer Management (Mach-based)
 * =============================================================================
 */

int platform_timer_create(uint64_t interval_ns) {
    if (!g_platform_initialized) {
        if (platform_init() != 0) {
            return -1;
        }
    }
    
    if (g_sampler_active) {
        return -1;  /* Already running */
    }
    
    if (g_ringbuffer == NULL) {
        return -1;  /* No ring buffer configured */
    }
    
    g_interval_ns = interval_ns;
    
    /* Start Mach-based sampler.
     * This creates a sampler thread that uses suspend-walk-resume pattern. */
    if (mach_sampler_start(interval_ns, g_ringbuffer) != 0) {
        return -1;
    }
    
    g_sampler_active = 1;
    return 0;
}

int platform_timer_destroy(void) {
    if (!g_sampler_active) {
        return 0;
    }
    
    /*
     * Release GIL before stopping sampler to avoid deadlock.
     * The sampler thread may be waiting to acquire the GIL via PyGILState_Ensure.
     * If we hold the GIL and call pthread_join, we deadlock.
     *
     * SAFETY: Check if current thread holds the GIL before using
     * Py_BEGIN_ALLOW_THREADS. This function may be called from:
     *   - Normal Python context (holds GIL) -> use Py_BEGIN_ALLOW_THREADS
     *   - atexit handlers or finalizers (may not hold GIL) -> call directly
     *
     * PyGILState_Check() returns 1 if current thread holds GIL, 0 otherwise.
     */
    if (PyGILState_Check()) {
        Py_BEGIN_ALLOW_THREADS
        mach_sampler_stop();
        Py_END_ALLOW_THREADS
    } else {
        /* GIL not held - safe to call directly without releasing */
        mach_sampler_stop();
    }
    
    g_sampler_active = 0;
    return 0;
}

int platform_timer_pause(void) {
    /* For now, pause = stop. A more sophisticated implementation could
     * signal the sampler thread to pause without terminating. */
    return platform_timer_destroy();
}

int platform_timer_resume(void) {
    /* Resume by restarting with saved interval */
    if (g_interval_ns == 0 || g_ringbuffer == NULL) {
        return -1;
    }
    return platform_timer_create(g_interval_ns);
}

/*
 * =============================================================================
 * Thread Management
 * =============================================================================
 */

int platform_register_thread(uint64_t interval_ns) {
    /* Mach sampler automatically discovers threads via pthread introspection hook.
     * No manual registration needed. */
    (void)interval_ns;
    return 0;
}

int platform_unregister_thread(void) {
    /* Mach sampler automatically tracks thread termination via introspection hook. */
    return 0;
}

/*
 * =============================================================================
 * Utility Functions
 * =============================================================================
 */

uint64_t platform_thread_id(void) {
    uint64_t tid;
    pthread_threadid_np(pthread_self(), &tid);
    return tid;
}

uint64_t platform_monotonic_ns(void) {
    if (!g_timebase_initialized) {
        mach_timebase_info(&g_timebase_info);
        g_timebase_initialized = 1;
    }

    uint64_t mach_time = mach_absolute_time();
    return mach_time * g_timebase_info.numer / g_timebase_info.denom;
}

const char* platform_name(void) {
    return SPPROF_PLATFORM_NAME;
}

/*
 * =============================================================================
 * Signal Handler (Compatibility stubs)
 * =============================================================================
 *
 * These functions exist for API compatibility but do nothing on Darwin
 * since we use Mach-based sampling instead of signals.
 */

int platform_set_signal_handler(void (*handler)(int, siginfo_t*, void*)) {
    /* Not used on Darwin - Mach sampler handles everything */
    (void)handler;
    return 0;
}

int platform_restore_signal_handler(void) {
    /* Not used on Darwin */
    return 0;
}

/*
 * =============================================================================
 * Statistics
 * =============================================================================
 */

void platform_get_stats(
    uint64_t* samples_captured,
    uint64_t* samples_dropped,
    uint64_t* timer_overruns
) {
    /* Get stats from Mach sampler */
    mach_sampler_get_stats(samples_captured, samples_dropped, NULL);
    
    if (timer_overruns) {
        *timer_overruns = 0;  /* Mach sampler doesn't have timer overruns */
    }
}

#endif /* __APPLE__ */
