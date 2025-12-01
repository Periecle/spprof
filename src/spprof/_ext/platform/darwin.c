/**
 * platform/darwin.c - macOS platform implementation
 *
 * Uses setitimer(ITIMER_PROF) for SIGPROF delivery.
 * Note: setitimer delivers signals to the process, not per-thread.
 *
 * Now uses the unified signal_handler.c for consistent behavior.
 */

#ifdef __APPLE__

#include <Python.h>
#include <signal.h>
#include <sys/time.h>
#include <pthread.h>
#include <mach/mach_time.h>
#include <string.h>

#include "platform.h"
#include "../ringbuffer.h"
#include "../framewalker.h"
#include "../signal_handler.h"

/* Forward declaration */
extern RingBuffer* g_ringbuffer;

/* Global state */
static uint64_t g_interval_ns = 0;
static int g_platform_initialized = 0;

/* Mach timebase for high-resolution timing */
static mach_timebase_info_data_t g_timebase_info;
static int g_timebase_initialized = 0;

int platform_init(void) {
    if (g_platform_initialized) {
        return 0;
    }
    
    if (!g_timebase_initialized) {
        mach_timebase_info(&g_timebase_info);
        g_timebase_initialized = 1;
    }
    
    g_platform_initialized = 1;
    return 0;
}

void platform_cleanup(void) {
    platform_timer_destroy();
    g_platform_initialized = 0;
}

int platform_timer_create(uint64_t interval_ns) {
    if (!g_platform_initialized) {
        return -1;
    }
    
    g_interval_ns = interval_ns;

    /* Install the unified signal handler */
    if (signal_handler_install(SPPROF_SIGNAL) < 0) {
        return -1;
    }

    /* Start accepting samples */
    signal_handler_start();

    /* Set up interval timer */
    struct itimerval it;
    uint64_t interval_us = interval_ns / 1000;

    it.it_value.tv_sec = interval_us / 1000000;
    it.it_value.tv_usec = interval_us % 1000000;
    it.it_interval = it.it_value;

    if (setitimer(ITIMER_PROF, &it, NULL) < 0) {
        signal_handler_stop();
        signal_handler_uninstall(SPPROF_SIGNAL);
        return -1;
    }

    return 0;
}

int platform_timer_destroy(void) {
    /* Stop the timer first */
    struct itimerval it;
    memset(&it, 0, sizeof(it));
    setitimer(ITIMER_PROF, &it, NULL);

    /* Stop accepting samples */
    signal_handler_stop();
    
    /* Brief pause for in-flight signals */
    struct timespec ts = {0, 1000000};  /* 1ms */
    nanosleep(&ts, NULL);
    
    /* Restore original signal handler */
    signal_handler_uninstall(SPPROF_SIGNAL);

    return 0;
}

int platform_timer_pause(void) {
    /* macOS: Not implemented - return success (no-op) */
    return 0;
}

int platform_timer_resume(void) {
    /* macOS: Not implemented - return success (no-op) */
    return 0;
}

uint64_t platform_thread_id(void) {
    uint64_t tid;
    pthread_threadid_np(pthread_self(), &tid);
    return tid;
}

uint64_t platform_monotonic_ns(void) {
    if (!g_timebase_initialized) {
        platform_init();
    }

    uint64_t mach_time = mach_absolute_time();
    return mach_time * g_timebase_info.numer / g_timebase_info.denom;
}

const char* platform_name(void) {
    return SPPROF_PLATFORM_NAME;
}

int platform_register_thread(uint64_t interval_ns) {
    /* macOS setitimer is process-wide, not per-thread */
    /* New threads automatically receive signals */
    (void)interval_ns;
    return 0;
}

int platform_unregister_thread(void) {
    return 0;
}

int platform_set_signal_handler(void (*handler)(int, siginfo_t*, void*)) {
    /* Using unified signal handler now */
    (void)handler;
    return 0;
}

int platform_restore_signal_handler(void) {
    return signal_handler_uninstall(SPPROF_SIGNAL);
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
    if (samples_captured) {
        *samples_captured = signal_handler_samples_captured();
    }
    if (samples_dropped) {
        *samples_dropped = signal_handler_samples_dropped();
    }
    if (timer_overruns) {
        *timer_overruns = 0;  /* setitimer doesn't track overruns */
    }
}

#endif /* __APPLE__ */

