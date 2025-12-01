/**
 * platform/linux.c - Linux platform implementation (production-ready)
 *
 * Uses timer_create() with SIGEV_THREAD_ID for per-thread CPU time sampling.
 * This provides the highest-fidelity profiling on Linux.
 *
 * Key features:
 *   - Per-thread timers using CLOCK_THREAD_CPUTIME_ID
 *   - CPU time sampling (only counts when thread is executing)
 *   - Dynamic thread registry with O(1) lookup via uthash
 *   - No artificial thread limits (supports 500+ threads)
 *   - Timer overrun tracking for profiling accuracy assessment
 *   - Race-free shutdown with signal blocking
 *   - Pause/resume support without timer recreation
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
#include <stdatomic.h>

#include "platform.h"
#include "../ringbuffer.h"
#include "../framewalker.h"
#include "../signal_handler.h"
#include "../uthash.h"

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

/*
 * =============================================================================
 * Thread Timer Registry (uthash-based)
 * =============================================================================
 */

/**
 * ThreadTimerEntry - Represents a single thread's timer state in the registry.
 *
 * Uses uthash for O(1) average case operations (add/find/delete).
 * Memory is dynamically allocated - no artificial thread limits.
 */
typedef struct ThreadTimerEntry {
    pid_t tid;              /* Key: Linux thread ID (gettid()) */
    timer_t timer_id;       /* POSIX timer handle */
    uint64_t overruns;      /* Accumulated timer overruns for this thread */
    int active;             /* 1 if timer is running, 0 if paused/stopped */
    UT_hash_handle hh;      /* uthash: makes structure hashable */
} ThreadTimerEntry;

/* Registry head pointer - uthash manages the hash table internally */
static ThreadTimerEntry* g_thread_registry = NULL;

/* RWLock for thread-safe registry access */
static pthread_rwlock_t g_registry_lock = PTHREAD_RWLOCK_INITIALIZER;

/*
 * =============================================================================
 * Global State
 * =============================================================================
 */

/* Timer state */
static timer_t g_main_timer = NULL;
static int g_platform_initialized = 0;
static uint64_t g_interval_ns = 0;

/* Pause state */
static int g_paused = 0;
static uint64_t g_saved_interval_ns = 0;

/* Statistics (atomic for signal-safe reads) */
static _Atomic uint64_t g_total_overruns = 0;
static _Atomic uint64_t g_timer_create_failures = 0;

/* Thread-local timer for fast path access */
static __thread timer_t tl_timer_id = NULL;
static __thread int tl_timer_active = 0;

/*
 * =============================================================================
 * Registry Management Functions
 * =============================================================================
 */

/**
 * Initialize the thread timer registry.
 *
 * Must be called once during platform_init().
 * NOT thread-safe - call only from main thread during startup.
 *
 * @return 0 on success, -1 on error
 */
static int registry_init(void) {
    /* RWLock is already statically initialized with PTHREAD_RWLOCK_INITIALIZER */
    
    /* Set registry head to NULL (empty) */
    g_thread_registry = NULL;
    
    /* Reset statistics */
    atomic_store(&g_total_overruns, 0);
    atomic_store(&g_timer_create_failures, 0);
    
    return 0;
}

/**
 * Clean up the thread timer registry.
 *
 * Deletes all timers and frees all memory.
 * Must block SIGPROF before calling.
 * NOT thread-safe - call only during shutdown.
 */
static void registry_cleanup(void) {
    sigset_t block_set, old_set;
    
    /* Block SIGPROF to prevent signal delivery during cleanup */
    sigemptyset(&block_set);
    sigaddset(&block_set, SPPROF_SIGNAL);
    pthread_sigmask(SIG_BLOCK, &block_set, &old_set);
    
    pthread_rwlock_wrlock(&g_registry_lock);
    
    ThreadTimerEntry *entry, *tmp;
    HASH_ITER(hh, g_thread_registry, entry, tmp) {
        /* Capture final overrun count before deletion */
        if (entry->timer_id != NULL) {
            int overrun = timer_getoverrun(entry->timer_id);
            if (overrun > 0) {
                atomic_fetch_add(&g_total_overruns, (uint64_t)overrun);
            }
            timer_delete(entry->timer_id);
        }
        HASH_DEL(g_thread_registry, entry);
        free(entry);
    }
    g_thread_registry = NULL;
    
    pthread_rwlock_unlock(&g_registry_lock);
    
    /* Drain any pending signals */
    struct timespec timeout = {0, 0};
    siginfo_t info;
    while (sigtimedwait(&block_set, &info, &timeout) > 0) {
        /* Discard pending SIGPROF */
    }
    
    /* Restore original signal mask */
    pthread_sigmask(SIG_SETMASK, &old_set, NULL);
}

/**
 * Add a thread timer entry to the registry.
 *
 * Thread-safe (acquires write lock).
 *
 * @param tid Thread ID (from gettid())
 * @param timer_id POSIX timer handle from timer_create()
 * @return 0 on success, -1 on error (ENOMEM or duplicate TID)
 */
static int registry_add_thread(pid_t tid, timer_t timer_id) {
    ThreadTimerEntry* entry = malloc(sizeof(ThreadTimerEntry));
    if (!entry) {
        errno = ENOMEM;
        return -1;
    }
    
    entry->tid = tid;
    entry->timer_id = timer_id;
    entry->overruns = 0;
    entry->active = 1;
    
    pthread_rwlock_wrlock(&g_registry_lock);
    
    /* Check for duplicate TID */
    ThreadTimerEntry* existing = NULL;
    HASH_FIND_INT(g_thread_registry, &tid, existing);
    if (existing) {
        pthread_rwlock_unlock(&g_registry_lock);
        free(entry);
        errno = EEXIST;
        return -1;
    }
    
    HASH_ADD_INT(g_thread_registry, tid, entry);
    pthread_rwlock_unlock(&g_registry_lock);
    
    return 0;
}

/**
 * Find a thread timer entry by TID.
 *
 * Thread-safe (acquires read lock).
 * Returns pointer to entry - do NOT free or modify without lock.
 *
 * @param tid Thread ID to look up
 * @return Pointer to entry, or NULL if not found
 */
static ThreadTimerEntry* registry_find_thread(pid_t tid) {
    ThreadTimerEntry* entry = NULL;
    pthread_rwlock_rdlock(&g_registry_lock);
    HASH_FIND_INT(g_thread_registry, &tid, entry);
    pthread_rwlock_unlock(&g_registry_lock);
    return entry;
}

/**
 * Remove a thread timer entry from the registry.
 *
 * Also deletes the associated POSIX timer.
 * Thread-safe (acquires write lock).
 *
 * @param tid Thread ID to remove
 * @return 0 on success, -1 if not found
 */
static int registry_remove_thread(pid_t tid) {
    ThreadTimerEntry* entry = NULL;
    
    pthread_rwlock_wrlock(&g_registry_lock);
    HASH_FIND_INT(g_thread_registry, &tid, entry);
    if (entry) {
        HASH_DEL(g_thread_registry, entry);
    }
    pthread_rwlock_unlock(&g_registry_lock);
    
    if (entry) {
        /* Capture final overrun count before deletion */
        if (entry->timer_id != NULL) {
            int overrun = timer_getoverrun(entry->timer_id);
            if (overrun > 0) {
                atomic_fetch_add(&g_total_overruns, (uint64_t)overrun);
            }
            timer_delete(entry->timer_id);
        }
        free(entry);
        return 0;
    }
    
    errno = ENOENT;
    return -1;
}

/**
 * Get the number of registered thread timers.
 *
 * Thread-safe (acquires read lock).
 *
 * @return Number of entries in registry
 */
static size_t registry_count(void) {
    size_t count;
    pthread_rwlock_rdlock(&g_registry_lock);
    count = HASH_COUNT(g_thread_registry);
    pthread_rwlock_unlock(&g_registry_lock);
    return count;
}

/*
 * =============================================================================
 * Statistics Functions
 * =============================================================================
 */

/**
 * Get total timer overruns across all threads.
 *
 * @return Sum of all timer overruns
 */
static uint64_t registry_get_total_overruns(void) {
    return atomic_load(&g_total_overruns);
}

/**
 * Add to the global overrun counter.
 *
 * Called by sample consumer when processing samples with overrun data.
 * Thread-safe (atomic operation).
 *
 * @param count Number of overruns to add
 */
static void registry_add_overruns(uint64_t count) {
    atomic_fetch_add(&g_total_overruns, count);
}

/**
 * Get count of timer creation failures.
 *
 * @return Number of failed timer_create() calls
 */
static uint64_t registry_get_create_failures(void) {
    return atomic_load(&g_timer_create_failures);
}

/*
 * =============================================================================
 * Pause/Resume Helpers
 * =============================================================================
 */

/**
 * Pause all registered thread timers.
 *
 * Sets timer interval to zero (disarms) without deleting.
 * Thread-safe (acquires read lock).
 *
 * @return 0 on success, -1 on error
 */
static int registry_pause_all(void) {
    struct itimerspec zero = {0};
    
    pthread_rwlock_wrlock(&g_registry_lock);
    ThreadTimerEntry *entry, *tmp;
    HASH_ITER(hh, g_thread_registry, entry, tmp) {
        if (entry->active && entry->timer_id != NULL) {
            timer_settime(entry->timer_id, 0, &zero, NULL);
            entry->active = 0;
        }
    }
    pthread_rwlock_unlock(&g_registry_lock);
    
    return 0;
}

/**
 * Resume all paused thread timers.
 *
 * Restores saved interval to all paused timers.
 * Thread-safe (acquires read lock).
 *
 * @param interval_ns Interval to set (nanoseconds)
 * @return 0 on success, -1 on error
 */
static int registry_resume_all(uint64_t interval_ns) {
    struct itimerspec its;
    its.it_value.tv_sec = interval_ns / 1000000000ULL;
    its.it_value.tv_nsec = interval_ns % 1000000000ULL;
    its.it_interval = its.it_value;
    
    pthread_rwlock_wrlock(&g_registry_lock);
    ThreadTimerEntry *entry, *tmp;
    HASH_ITER(hh, g_thread_registry, entry, tmp) {
        if (!entry->active && entry->timer_id != NULL) {
            timer_settime(entry->timer_id, 0, &its, NULL);
            entry->active = 1;
        }
    }
    pthread_rwlock_unlock(&g_registry_lock);
    
    return 0;
}

/*
 * =============================================================================
 * Platform Initialization
 * =============================================================================
 */

int platform_init(void) {
    if (g_platform_initialized) {
        return 0;
    }
    
    /* Initialize thread timer registry */
    registry_init();
    
    /* Reset pause state */
    g_paused = 0;
    g_saved_interval_ns = 0;
    
    g_platform_initialized = 1;
    return 0;
}

void platform_cleanup(void) {
    platform_timer_destroy();
    
    /* Clean up thread registry (handles signal blocking internally) */
    registry_cleanup();
    
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
    pid_t tid = syscall(SYS_gettid);
    struct sigevent sev;
    memset(&sev, 0, sizeof(sev));
    sev.sigev_notify = SIGEV_THREAD_ID;
    sev.sigev_signo = SPPROF_SIGNAL;
    sev._sigev_un._tid = tid;
    
    if (timer_create(CLOCK_THREAD_CPUTIME_ID, &sev, &g_main_timer) < 0) {
        atomic_fetch_add(&g_timer_create_failures, 1);
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
    
    /* Track main thread timer in registry */
    registry_add_thread(tid, g_main_timer);
    
    return 0;
}

int platform_timer_destroy(void) {
    sigset_t block_set, old_set;
    
    /* Block SIGPROF to prevent signal delivery during cleanup */
    sigemptyset(&block_set);
    sigaddset(&block_set, SPPROF_SIGNAL);
    pthread_sigmask(SIG_BLOCK, &block_set, &old_set);
    
    /* Stop accepting samples */
    signal_handler_stop();
    
    /* Capture final overrun count from main timer */
    if (g_main_timer != NULL) {
        int overrun = timer_getoverrun(g_main_timer);
        if (overrun > 0) {
            atomic_fetch_add(&g_total_overruns, (uint64_t)overrun);
        }
        timer_delete(g_main_timer);
        g_main_timer = NULL;
    }
    
    /* Delete thread-local timer if any */
    if (tl_timer_active && tl_timer_id != NULL) {
        int overrun = timer_getoverrun(tl_timer_id);
        if (overrun > 0) {
            atomic_fetch_add(&g_total_overruns, (uint64_t)overrun);
        }
        timer_delete(tl_timer_id);
        tl_timer_id = NULL;
        tl_timer_active = 0;
    }
    
    /* Drain any pending signals */
    struct timespec timeout = {0, 0};
    siginfo_t info;
    while (sigtimedwait(&block_set, &info, &timeout) > 0) {
        /* Discard pending SIGPROF */
    }
    
    /* Restore original signal mask */
    pthread_sigmask(SIG_SETMASK, &old_set, NULL);
    
    /* Now safe to restore original signal handler */
    signal_handler_uninstall(SPPROF_SIGNAL);
    
    return 0;
}

/*
 * =============================================================================
 * Pause/Resume Support
 * =============================================================================
 */

/**
 * Pause all profiling timers.
 *
 * Disarms timers by setting zero interval. Timers remain allocated.
 *
 * @return 0 on success, -1 on error
 */
int platform_timer_pause(void) {
    if (g_paused || g_main_timer == NULL) {
        return 0;  /* Already paused or no timer */
    }
    
    /* Save current interval */
    struct itimerspec current;
    if (timer_gettime(g_main_timer, &current) < 0) {
        return -1;
    }
    g_saved_interval_ns = current.it_interval.tv_sec * 1000000000ULL 
                        + current.it_interval.tv_nsec;
    
    /* Disarm main timer */
    struct itimerspec zero = {0};
    if (timer_settime(g_main_timer, 0, &zero, NULL) < 0) {
        return -1;
    }
    
    /* Pause all registered thread timers */
    registry_pause_all();
    
    /* Stop accepting samples */
    signal_handler_stop();
    g_paused = 1;
    
    return 0;
}

/**
 * Resume all paused profiling timers.
 *
 * Restores saved interval to all timers.
 *
 * @return 0 on success, -1 on error
 */
int platform_timer_resume(void) {
    if (!g_paused || g_main_timer == NULL) {
        return 0;  /* Not paused or no timer */
    }
    
    /* Start accepting samples */
    signal_handler_start();
    
    /* Restore main timer interval */
    struct itimerspec its;
    its.it_value.tv_sec = g_saved_interval_ns / 1000000000ULL;
    its.it_value.tv_nsec = g_saved_interval_ns % 1000000000ULL;
    its.it_interval = its.it_value;
    
    if (timer_settime(g_main_timer, 0, &its, NULL) < 0) {
        return -1;
    }
    
    /* Resume all registered thread timers */
    registry_resume_all(g_saved_interval_ns);
    
    g_paused = 0;
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
 *
 * Includes retry logic for transient failures (EAGAIN).
 */
int platform_register_thread(uint64_t interval_ns) {
    if (tl_timer_active) {
        return 0;  /* Already registered */
    }
    
    pid_t tid = syscall(SYS_gettid);
    
    /* Check if already in registry (e.g., re-registration) */
    if (registry_find_thread(tid) != NULL) {
        return 0;
    }
    
    /* Create thread-specific timer */
    struct sigevent sev;
    memset(&sev, 0, sizeof(sev));
    sev.sigev_notify = SIGEV_THREAD_ID;
    sev.sigev_signo = SPPROF_SIGNAL;
    sev._sigev_un._tid = tid;
    
    timer_t timer_id = NULL;
    int create_result = timer_create(CLOCK_THREAD_CPUTIME_ID, &sev, &timer_id);
    
    /* Retry once on EAGAIN */
    if (create_result < 0 && errno == EAGAIN) {
        usleep(1000);  /* 1ms delay */
        create_result = timer_create(CLOCK_THREAD_CPUTIME_ID, &sev, &timer_id);
    }
    
    if (create_result < 0) {
        atomic_fetch_add(&g_timer_create_failures, 1);
        return -1;
    }
    
    /* Configure and start timer */
    struct itimerspec its;
    its.it_value.tv_sec = interval_ns / 1000000000ULL;
    its.it_value.tv_nsec = interval_ns % 1000000000ULL;
    its.it_interval = its.it_value;
    
    if (timer_settime(timer_id, 0, &its, NULL) < 0) {
        timer_delete(timer_id);
        return -1;
    }
    
    /* Update TLS (fast path) */
    tl_timer_id = timer_id;
    tl_timer_active = 1;
    
    /* Update registry (management path) */
    if (registry_add_thread(tid, timer_id) < 0) {
        /* Registry add failed (e.g., duplicate) - still continue with TLS */
    }
    
    return 0;
}

int platform_unregister_thread(void) {
    if (!tl_timer_active) {
        return 0;
    }
    
    pid_t tid = syscall(SYS_gettid);
    
    /* Remove from registry (handles timer deletion and overrun capture) */
    registry_remove_thread(tid);
    
    /* Clear TLS */
    tl_timer_id = NULL;
    tl_timer_active = 0;
    
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
    if (timer_overruns) {
        *timer_overruns = registry_get_total_overruns();
    }
}

/**
 * Get extended platform-specific statistics.
 *
 * @param samples_captured Output: number of samples captured
 * @param samples_dropped Output: number of samples dropped
 * @param timer_overruns Output: total timer overruns
 * @param timer_create_failures Output: failed timer_create() calls
 * @param registered_threads Output: number of registered threads
 */
void platform_get_extended_stats(
    uint64_t* samples_captured,
    uint64_t* samples_dropped,
    uint64_t* timer_overruns,
    uint64_t* timer_create_failures,
    uint64_t* registered_threads
) {
    if (samples_captured) {
        *samples_captured = signal_handler_samples_captured();
    }
    if (samples_dropped) {
        *samples_dropped = signal_handler_samples_dropped();
    }
    if (timer_overruns) {
        *timer_overruns = registry_get_total_overruns();
    }
    if (timer_create_failures) {
        *timer_create_failures = registry_get_create_failures();
    }
    if (registered_threads) {
        *registered_threads = (uint64_t)registry_count();
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
    fprintf(stderr, "  Paused: %d\n", g_paused);
    fprintf(stderr, "  Signal: %d (SIGPROF=%d)\n", SPPROF_SIGNAL, SIGPROF);
    
    pthread_rwlock_rdlock(&g_registry_lock);
    size_t count = HASH_COUNT(g_thread_registry);
    fprintf(stderr, "  Registered threads: %zu\n", count);
    
    ThreadTimerEntry *entry, *tmp;
    int idx = 0;
    HASH_ITER(hh, g_thread_registry, entry, tmp) {
        fprintf(stderr, "    Thread %d: tid=%d, active=%d, overruns=%llu\n",
                idx++, entry->tid, entry->active, 
                (unsigned long long)entry->overruns);
    }
    pthread_rwlock_unlock(&g_registry_lock);
    
    fprintf(stderr, "  Total overruns: %llu\n", 
            (unsigned long long)registry_get_total_overruns());
    fprintf(stderr, "  Timer create failures: %llu\n",
            (unsigned long long)registry_get_create_failures());
    
    signal_handler_debug_info();
}

#endif /* SPPROF_DEBUG */

#endif /* __linux__ */
