/**
 * platform/platform.h - Platform abstraction layer
 *
 * Provides cross-platform interfaces for:
 *   - Timer management (per-thread CPU time sampling)
 *   - Thread identification
 *   - Monotonic time
 *   - Signal handling
 *
 * Copyright (c) 2024 spprof contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef SPPROF_PLATFORM_H
#define SPPROF_PLATFORM_H

#include <stdint.h>
#include <signal.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * =============================================================================
 * Platform Detection
 * =============================================================================
 */

#if defined(__linux__)
    #define SPPROF_PLATFORM_LINUX 1
    #define SPPROF_PLATFORM_NAME "linux"
    #define SPPROF_SIGNAL SIGPROF
#elif defined(__APPLE__) && defined(__MACH__)
    #define SPPROF_PLATFORM_DARWIN 1
    #define SPPROF_PLATFORM_NAME "darwin"
    #define SPPROF_SIGNAL SIGPROF
#elif defined(_WIN32) || defined(_WIN64)
    #define SPPROF_PLATFORM_WINDOWS 1
    #define SPPROF_PLATFORM_NAME "windows"
    /* Windows doesn't use signals for profiling */
    #define SPPROF_SIGNAL 0
#else
    #define SPPROF_PLATFORM_UNKNOWN 1
    #define SPPROF_PLATFORM_NAME "unknown"
    #define SPPROF_SIGNAL SIGPROF
#endif

/*
 * =============================================================================
 * Platform Initialization
 * =============================================================================
 */

/**
 * Initialize platform-specific subsystems.
 *
 * Must be called before any other platform functions.
 * NOT async-signal-safe.
 *
 * @return 0 on success, -1 on error
 */
int platform_init(void);

/**
 * Clean up platform-specific resources.
 *
 * Call during module shutdown.
 */
void platform_cleanup(void);

/*
 * =============================================================================
 * Timer Management
 * =============================================================================
 */

/**
 * Create and start a profiling timer.
 *
 * Creates a per-thread timer that fires at the specified interval,
 * sending SPPROF_SIGNAL to the calling thread.
 *
 * @param interval_ns Timer interval in nanoseconds
 * @return 0 on success, -1 on error
 */
int platform_timer_create(uint64_t interval_ns);

/**
 * Stop and destroy the profiling timer.
 *
 * @return 0 on success, -1 on error
 */
int platform_timer_destroy(void);

/**
 * Register a thread for per-thread sampling.
 *
 * On Linux with timer_create/SIGEV_THREAD_ID, each thread needs
 * its own timer. Call this from each thread to be profiled.
 *
 * @param interval_ns Timer interval in nanoseconds
 * @return 0 on success, -1 on error
 */
int platform_register_thread(uint64_t interval_ns);

/**
 * Unregister a thread from sampling.
 *
 * Call when a profiled thread is about to exit.
 *
 * @return 0 on success, -1 on error
 */
int platform_unregister_thread(void);

/*
 * =============================================================================
 * Signal Handler Management
 * =============================================================================
 */

/**
 * Set custom signal handler (deprecated - use signal_handler.c).
 *
 * @param handler Signal handler function
 * @return 0 on success, -1 on error
 */
int platform_set_signal_handler(void (*handler)(int, siginfo_t*, void*));

/**
 * Restore original signal handler.
 *
 * @return 0 on success, -1 on error
 */
int platform_restore_signal_handler(void);

/*
 * =============================================================================
 * Utility Functions
 * =============================================================================
 */

/**
 * Get current thread ID.
 *
 * ASYNC-SIGNAL-SAFE on most platforms.
 *
 * @return Thread ID
 */
uint64_t platform_thread_id(void);

/**
 * Get monotonic timestamp in nanoseconds.
 *
 * ASYNC-SIGNAL-SAFE.
 *
 * @return Nanoseconds since some unspecified epoch
 */
uint64_t platform_monotonic_ns(void);

/**
 * Get platform name string.
 *
 * @return Platform name ("linux", "darwin", "windows")
 */
const char* platform_name(void);

/*
 * =============================================================================
 * Statistics (optional)
 * =============================================================================
 */

/**
 * Get platform-specific statistics.
 *
 * @param samples_captured Output: number of samples captured
 * @param samples_dropped Output: number of samples dropped
 * @param timer_overruns Output: number of timer overruns (Linux)
 */
void platform_get_stats(
    uint64_t* samples_captured,
    uint64_t* samples_dropped,
    uint64_t* timer_overruns
);

/*
 * =============================================================================
 * Debug Support
 * =============================================================================
 */

#ifdef SPPROF_DEBUG
void platform_debug_info(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* SPPROF_PLATFORM_H */
