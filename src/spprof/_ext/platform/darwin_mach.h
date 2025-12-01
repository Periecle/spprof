/**
 * darwin_mach.h - Darwin Mach-based sampler public interface
 *
 * Provides Mach kernel-based sampling using the "Suspend-Walk-Resume" pattern.
 * This replaces setitimer(ITIMER_PROF) with:
 *   1. Thread discovery via pthread_introspection_hook_install
 *   2. Thread suspension via thread_suspend()
 *   3. Register capture via thread_get_state()
 *   4. Frame pointer walking
 *   5. Thread resume via thread_resume()
 *
 * This approach provides accurate per-thread sampling and eliminates
 * signal-safety constraints.
 *
 * ERROR HANDLING CONVENTIONS (see error.h for full documentation):
 *
 * This module uses POSIX-style error handling with errno for detailed errors:
 *
 *   Pattern 1 - POSIX-style (0 success, -1 error with errno):
 *     - mach_sampler_init()   → sets ENOMEM, EBUSY
 *     - mach_sampler_start()  → sets EINVAL, EAGAIN, ENOMEM
 *     - mach_sampler_stop()   → sets ESRCH
 *
 *   Pattern 2 - Boolean query (1 true, 0 false):
 *     - mach_sampler_get_native_unwinding()
 *
 * The errno-based approach is used here because:
 *   1. Mach kernel errors map naturally to errno codes
 *   2. POSIX is the standard convention for system-level C APIs
 *   3. Callers can use standard perror() or strerror() for diagnostics
 *
 * Copyright (c) 2024 spprof contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef SPPROF_DARWIN_MACH_H
#define SPPROF_DARWIN_MACH_H

#ifdef __APPLE__

#include <stdint.h>
#include <stddef.h>

/* Include ringbuffer for RingBuffer type */
#include "../ringbuffer.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * =============================================================================
 * Initialization
 * =============================================================================
 */

/**
 * Initialize the Mach sampler subsystem.
 *
 * Must be called before any other mach_sampler_* functions.
 * Installs the pthread introspection hook for thread tracking.
 *
 * Thread safety: NOT thread-safe. Call once at module init.
 *
 * Error handling: POSIX-style with errno (Pattern 1)
 *   Returns 0 on success
 *   Returns -1 on error, sets errno to:
 *     ENOMEM - memory allocation failed
 *     EBUSY  - already initialized
 *
 * @return 0 on success, -1 on error (check errno for details).
 */
int mach_sampler_init(void);

/**
 * Clean up the Mach sampler subsystem.
 *
 * Stops sampling if running, removes hooks, frees resources.
 *
 * Thread safety: NOT thread-safe. Call once at module cleanup.
 */
void mach_sampler_cleanup(void);

/*
 * =============================================================================
 * Sampling Control
 * =============================================================================
 */

/**
 * Start sampling.
 *
 * Creates and starts the sampler thread. Sampling begins immediately.
 *
 * Error handling: POSIX-style with errno (Pattern 1)
 *   Returns 0 on success
 *   Returns -1 on error, sets errno to:
 *     EINVAL - invalid interval or NULL ringbuffer
 *     EAGAIN - sampler already running
 *     ENOMEM - thread creation failed
 *
 * @param interval_ns  Sampling interval in nanoseconds (1ms - 1s)
 * @param ringbuffer   Ring buffer to write samples to (must outlive sampler)
 *
 * @return 0 on success, -1 on error (check errno for details).
 *
 * Preconditions:
 *   - mach_sampler_init() called
 *   - mach_sampler_start() not already called (or stopped)
 *
 * Postconditions:
 *   - Sampler thread running
 *   - Samples written to ringbuffer at interval_ns
 */
int mach_sampler_start(uint64_t interval_ns, RingBuffer* ringbuffer);

/**
 * Stop sampling.
 *
 * Signals the sampler thread to stop and waits for it to exit.
 * All suspended threads are guaranteed to be resumed before return.
 *
 * Thread safety: NOT thread-safe. Call from single control thread.
 *
 * Error handling: POSIX-style with errno (Pattern 1)
 *   Returns 0 on success
 *   Returns -1 on error, sets errno to:
 *     ESRCH - sampler not running
 *
 * @return 0 on success, -1 on error (check errno for details).
 *
 * Postconditions:
 *   - Sampler thread terminated
 *   - No threads left in suspended state
 *   - Statistics finalized
 */
int mach_sampler_stop(void);

/*
 * =============================================================================
 * Configuration
 * =============================================================================
 */

/**
 * Enable or disable native (C-stack) frame capture.
 *
 * When enabled, captured frames include native return addresses.
 * Can be called while sampler is running.
 *
 * @param enabled  1 to enable, 0 to disable
 */
void mach_sampler_set_native_unwinding(int enabled);

/**
 * Check if native unwinding is enabled.
 *
 * Error handling: Boolean query (Pattern 2)
 *   Returns 1 = true (native unwinding enabled)
 *   Returns 0 = false (native unwinding disabled)
 *
 * @return 1 if enabled, 0 if disabled.
 */
int mach_sampler_get_native_unwinding(void);

/*
 * =============================================================================
 * Statistics
 * =============================================================================
 */

/**
 * Get sampling statistics.
 *
 * Thread safety: Safe to call from any thread while sampler is running.
 * Statistics are approximate (no locking).
 *
 * @param samples_captured  Output: samples successfully written to ringbuffer
 * @param samples_dropped   Output: samples dropped due to full buffer
 * @param threads_sampled   Output: total thread samples taken
 *
 * Any output parameter may be NULL to skip.
 */
void mach_sampler_get_stats(
    uint64_t* samples_captured,
    uint64_t* samples_dropped,
    uint64_t* threads_sampled
);

/**
 * Get extended statistics including timing information.
 *
 * @param samples_captured   Output: samples written
 * @param samples_dropped    Output: samples dropped
 * @param threads_sampled    Output: total thread samples
 * @param threads_skipped    Output: threads skipped (terminated, invalid)
 * @param total_suspend_ns   Output: cumulative thread suspension time
 * @param max_suspend_ns     Output: maximum single suspension time
 * @param walk_errors        Output: frame walking errors
 */
void mach_sampler_get_extended_stats(
    uint64_t* samples_captured,
    uint64_t* samples_dropped,
    uint64_t* threads_sampled,
    uint64_t* threads_skipped,
    uint64_t* total_suspend_ns,
    uint64_t* max_suspend_ns,
    uint64_t* walk_errors
);

/*
 * =============================================================================
 * Thread Registry
 * =============================================================================
 */

/**
 * Get the number of threads currently tracked.
 *
 * Thread safety: Safe to call from any thread.
 *
 * @return Number of threads in registry
 */
size_t mach_sampler_thread_count(void);

#ifdef __cplusplus
}
#endif

#endif /* __APPLE__ */

#endif /* SPPROF_DARWIN_MACH_H */

