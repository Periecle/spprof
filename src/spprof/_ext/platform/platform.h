/**
 * platform/platform.h - Platform abstraction layer
 *
 * Provides cross-platform interfaces for:
 *   - Timer management (per-thread CPU time sampling)
 *   - Thread identification
 *   - Monotonic time
 *   - Signal handling (POSIX only)
 *
 * Copyright (c) 2024 spprof contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef SPPROF_PLATFORM_H
#define SPPROF_PLATFORM_H

#include <stdint.h>

/* Include signal.h only on POSIX systems */
#ifndef _WIN32
#include <signal.h>
#endif

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
 * Pause all profiling timers.
 *
 * Disarms timers by setting zero interval. Timers remain allocated.
 * On platforms that don't support this, returns 0 (no-op).
 *
 * @return 0 on success, -1 on error
 */
int platform_timer_pause(void);

/**
 * Resume all paused profiling timers.
 *
 * Restores saved interval to all timers.
 * On platforms that don't support this, returns 0 (no-op).
 *
 * @return 0 on success, -1 on error
 */
int platform_timer_resume(void);

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
 * Signal Handler Management (POSIX only)
 * =============================================================================
 */

#ifndef _WIN32
/**
 * Set custom signal handler (deprecated - use signal_handler.c).
 *
 * @param handler Signal handler function
 * @return 0 on success, -1 on error
 */
int platform_set_signal_handler(void (*handler)(int, siginfo_t*, void*));
#endif

/**
 * Restore original signal handler.
 *
 * @return 0 on success, -1 on error (no-op on Windows)
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
 * Linux-Specific Extensions
 * =============================================================================
 */

#ifdef SPPROF_PLATFORM_LINUX

/**
 * Get extended Linux-specific statistics.
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
);

#endif /* SPPROF_PLATFORM_LINUX */

/*
 * =============================================================================
 * Windows-Specific Extensions
 * =============================================================================
 */

#ifdef SPPROF_PLATFORM_WINDOWS

/**
 * Get extended Windows-specific statistics.
 *
 * @param samples_captured Output: number of samples captured
 * @param samples_dropped Output: number of samples dropped
 * @param timer_callbacks Output: number of timer callbacks executed
 * @param gil_wait_time_ns Output: total time waiting for GIL in nanoseconds
 */
void platform_get_extended_stats(
    uint64_t* samples_captured,
    uint64_t* samples_dropped,
    uint64_t* timer_callbacks,
    uint64_t* gil_wait_time_ns
);

/**
 * Enable or disable CPU time sampling (vs wall time).
 *
 * When enabled, timestamps are based on thread CPU time using GetThreadTimes()
 * instead of wall clock time. This is useful for profiling CPU-bound code.
 *
 * @param enabled 1 to enable, 0 to disable
 */
void platform_set_cpu_time(int enabled);

/**
 * Check if CPU time sampling is enabled.
 *
 * @return 1 if enabled, 0 if disabled
 */
int platform_get_cpu_time(void);

/**
 * Enable or disable native stack unwinding.
 *
 * When enabled, the profiler captures native C/C++ stack frames using
 * CaptureStackBackTrace() in addition to Python frames.
 *
 * @param enabled 1 to enable, 0 to disable
 */
void platform_set_native_unwinding(int enabled);

/**
 * Check if native unwinding is enabled.
 *
 * @return 1 if enabled, 0 if disabled
 */
int platform_get_native_unwinding(void);

/**
 * Enable or disable per-thread sampling mode.
 *
 * When enabled, each thread gets its own timer using CreateThreadpoolTimer
 * instead of a global timer that samples all threads.
 *
 * @param enabled 1 to enable, 0 to disable
 */
void platform_set_per_thread_mode(int enabled);

/**
 * Check if per-thread sampling mode is enabled.
 *
 * @return 1 if enabled, 0 if disabled
 */
int platform_get_per_thread_mode(void);

#endif /* SPPROF_PLATFORM_WINDOWS */

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
