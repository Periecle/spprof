/**
 * platform/linux.c - Linux platform implementation (production-ready)
 *
 * Uses timer_create() with SIGEV_THREAD_ID for per-thread CPU time sampling.
 * This provides the highest-fidelity profiling on Linux.
 *
 * Key features:
 *   - Per-thread timers using CLOCK_THREAD_CPUTIME_ID
 *   - CPU time sampling (only counts when thread is executing)
 *   - Integrates with async-signal-safe signal handler
 *
 * Copyright (c) 2024 spprof contributors
 * SPDX-License-Identifier: MIT
 */

#ifdef __linux__

#define _GNU_SOURCE
#include <Python.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>

#include "platform.h"
#include "../ringbuffer.h"
#include "../framewalker.h"
#include "../signal_handler.h"

/* Forward declaration of global ring buffer */
extern RingBuffer* g_ringbuffer;

/*
 * =============================================================================
 * Constants and Configuration
 * =============================================================================
 */

/* Default signal for profiling - SIGPROF is traditional for profilers */
#ifndef SPPROF_SIGNAL
#define SPPROF_SIGNAL SIGPROF
#endif

/* Maximum number of threads we can track */
#define MAX_TRACKED_THREADS 256

/*
 * =============================================================================
 * Global State
 * =============================================================================
 */

static timer_t g_main_timer = NULL;
static int g_platform_initialized = 0;
static uint64_t g_interval_ns = 0;

/* Thread tracking for per-thread timers */
typedef struct {
    timer_t timer_id;
    pid_t tid;
    int active;
} ThreadTimer;

static ThreadTimer g_thread_timers[MAX_TRACKED_THREADS];
static pthread_mutex_t g_thread_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_thread_count = 0;

/* Thread-local timer for new threads */
static __thread timer_t tl_timer_id = NULL;
static __thread int tl_timer_active = 0;

/*
 * =============================================================================
 * Platform Initialization
 * =============================================================================
 */

int platform_init(void) {
    if (g_platform_initialized) {
        return 0;
    }
    
    /* Initialize thread timer tracking */
    memset(g_thread_timers, 0, sizeof(g_thread_timers));
    g_thread_count = 0;
    
    g_platform_initialized = 1;
    return 0;
}

void platform_cleanup(void) {
    platform_timer_destroy();
    
    /* Clean up any remaining thread timers */
    pthread_mutex_lock(&g_thread_lock);
    for (int i = 0; i < g_thread_count; i++) {
        if (g_thread_timers[i].active && g_thread_timers[i].timer_id != NULL) {
            timer_delete(g_thread_timers[i].timer_id);
        }
    }
    g_thread_count = 0;
    pthread_mutex_unlock(&g_thread_lock);
    
    g_platform_initialized = 0;
}

/*
 * =============================================================================
 * Timer Management
 * =============================================================================
 */

/**
 * Create and start the main profiling timer.
 *
 * Uses timer_create with SIGEV_THREAD_ID to target the calling thread.
 * The signal handler (from signal_handler.c) captures the stack.
 */
int platform_timer_create(uint64_t interval_ns) {
    if (!g_platform_initialized) {
        return -1;
    }
    
    g_interval_ns = interval_ns;
    
    /* Install signal handler first */
    if (signal_handler_install(SPPROF_SIGNAL) < 0) {
        return -1;
    }
    
    /* Start accepting samples */
    signal_handler_start();
    
    /* Create timer for main thread */
    struct sigevent sev;
    memset(&sev, 0, sizeof(sev));
    sev.sigev_notify = SIGEV_THREAD_ID;
    sev.sigev_signo = SPPROF_SIGNAL;
    sev._sigev_un._tid = syscall(SYS_gettid);
    
    if (timer_create(CLOCK_THREAD_CPUTIME_ID, &sev, &g_main_timer) < 0) {
        signal_handler_stop();
        signal_handler_uninstall(SPPROF_SIGNAL);
        return -1;
    }
    
    /* Configure and start timer */
    struct itimerspec its;
    its.it_value.tv_sec = interval_ns / 1000000000ULL;
    its.it_value.tv_nsec = interval_ns % 1000000000ULL;
    its.it_interval = its.it_value;
    
    if (timer_settime(g_main_timer, 0, &its, NULL) < 0) {
        timer_delete(g_main_timer);
        g_main_timer = NULL;
        signal_handler_stop();
        signal_handler_uninstall(SPPROF_SIGNAL);
        return -1;
    }
    
    /* Track main thread timer */
    pthread_mutex_lock(&g_thread_lock);
    if (g_thread_count < MAX_TRACKED_THREADS) {
        g_thread_timers[g_thread_count].timer_id = g_main_timer;
        g_thread_timers[g_thread_count].tid = syscall(SYS_gettid);
        g_thread_timers[g_thread_count].active = 1;
        g_thread_count++;
    }
    pthread_mutex_unlock(&g_thread_lock);
    
    return 0;
}

int platform_timer_destroy(void) {
    /* IMPORTANT: Delete timers FIRST to stop signal generation */
    
    /* Delete main timer */
    if (g_main_timer != NULL) {
        timer_delete(g_main_timer);
        g_main_timer = NULL;
    }
    
    /* Delete thread-local timer if any */
    if (tl_timer_active && tl_timer_id != NULL) {
        timer_delete(tl_timer_id);
        tl_timer_id = NULL;
        tl_timer_active = 0;
    }
    
    /* Stop accepting samples (after timers deleted) */
    signal_handler_stop();
    
    /* Small delay to let any in-flight signals be handled */
    /* This is a workaround for the signal delivery race */
    struct timespec ts = {0, 1000000};  /* 1ms */
    nanosleep(&ts, NULL);
    
    /* Now safe to restore original signal handler */
    signal_handler_uninstall(SPPROF_SIGNAL);
    
    return 0;
}

/*
 * =============================================================================
 * Thread Management
 * =============================================================================
 */

/**
 * Register a new thread for sampling.
 *
 * Each thread needs its own timer with SIGEV_THREAD_ID.
 * Call this from each thread that should be profiled.
 */
int platform_register_thread(uint64_t interval_ns) {
    if (tl_timer_active) {
        return 0;  /* Already registered */
    }
    
    pid_t tid = syscall(SYS_gettid);
    
    /* Create thread-specific timer */
    struct sigevent sev;
    memset(&sev, 0, sizeof(sev));
    sev.sigev_notify = SIGEV_THREAD_ID;
    sev.sigev_signo = SPPROF_SIGNAL;
    sev._sigev_un._tid = tid;
    
    if (timer_create(CLOCK_THREAD_CPUTIME_ID, &sev, &tl_timer_id) < 0) {
        return -1;
    }
    
    /* Configure and start timer */
    struct itimerspec its;
    its.it_value.tv_sec = interval_ns / 1000000000ULL;
    its.it_value.tv_nsec = interval_ns % 1000000000ULL;
    its.it_interval = its.it_value;
    
    if (timer_settime(tl_timer_id, 0, &its, NULL) < 0) {
        timer_delete(tl_timer_id);
        tl_timer_id = NULL;
        return -1;
    }
    
    tl_timer_active = 1;
    
    /* Track this thread's timer */
    pthread_mutex_lock(&g_thread_lock);
    if (g_thread_count < MAX_TRACKED_THREADS) {
        g_thread_timers[g_thread_count].timer_id = tl_timer_id;
        g_thread_timers[g_thread_count].tid = tid;
        g_thread_timers[g_thread_count].active = 1;
        g_thread_count++;
    }
    pthread_mutex_unlock(&g_thread_lock);
    
    return 0;
}

int platform_unregister_thread(void) {
    if (!tl_timer_active) {
        return 0;
    }
    
    pid_t tid = syscall(SYS_gettid);
    
    /* Delete timer */
    if (tl_timer_id != NULL) {
        timer_delete(tl_timer_id);
        tl_timer_id = NULL;
    }
    tl_timer_active = 0;
    
    /* Remove from tracking */
    pthread_mutex_lock(&g_thread_lock);
    for (int i = 0; i < g_thread_count; i++) {
        if (g_thread_timers[i].tid == tid) {
            g_thread_timers[i].active = 0;
            break;
        }
    }
    pthread_mutex_unlock(&g_thread_lock);
    
    return 0;
}

/*
 * =============================================================================
 * Signal Handler Configuration
 * =============================================================================
 */

int platform_set_signal_handler(void (*handler)(int, siginfo_t*, void*)) {
    /* We use our own signal handler now - this is for backwards compatibility */
    (void)handler;
    return 0;
}

int platform_restore_signal_handler(void) {
    return signal_handler_uninstall(SPPROF_SIGNAL);
}

/*
 * =============================================================================
 * Utility Functions
 * =============================================================================
 */

uint64_t platform_thread_id(void) {
    return (uint64_t)syscall(SYS_gettid);
}

uint64_t platform_monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

const char* platform_name(void) {
    return SPPROF_PLATFORM_NAME;
}

/*
 * =============================================================================
 * Statistics
 * =============================================================================
 */

/**
 * Get platform-specific profiling statistics.
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
    if (timer_overruns && g_main_timer != NULL) {
        *timer_overruns = (uint64_t)timer_getoverrun(g_main_timer);
    }
}

/*
 * =============================================================================
 * Debug Support
 * =============================================================================
 */

#ifdef SPPROF_DEBUG

#include <stdio.h>

void platform_debug_info(void) {
    fprintf(stderr, "[spprof] Linux Platform Info:\n");
    fprintf(stderr, "  Initialized: %d\n", g_platform_initialized);
    fprintf(stderr, "  Main timer: %p\n", (void*)g_main_timer);
    fprintf(stderr, "  Interval: %llu ns (%.2f ms)\n", 
            (unsigned long long)g_interval_ns,
            (double)g_interval_ns / 1000000.0);
    fprintf(stderr, "  Tracked threads: %d\n", g_thread_count);
    fprintf(stderr, "  Signal: %d (SIGPROF=%d)\n", SPPROF_SIGNAL, SIGPROF);
    
    pthread_mutex_lock(&g_thread_lock);
    for (int i = 0; i < g_thread_count; i++) {
        fprintf(stderr, "    Thread %d: tid=%d, active=%d\n",
                i, g_thread_timers[i].tid, g_thread_timers[i].active);
    }
    pthread_mutex_unlock(&g_thread_lock);
    
    signal_handler_debug_info();
}

#endif /* SPPROF_DEBUG */

#endif /* __linux__ */
