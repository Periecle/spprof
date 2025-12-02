/**
 * signal_handler.h - Production async-signal-safe signal handler interface
 *
 * This module provides the signal handling infrastructure for sampling.
 * The actual signal handler is fully async-signal-safe.
 *
 * NOTE: On Windows, these functions are stubs implemented in platform/windows.c.
 * The actual sampling is done via Windows timer queue timers.
 *
 * Copyright (c) 2024 spprof contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef SPPROF_SIGNAL_HANDLER_H
#define SPPROF_SIGNAL_HANDLER_H

#include <stdint.h>

/* Include signal.h only on POSIX systems */
#ifndef _WIN32
#include <signal.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifndef _WIN32
/**
 * The actual signal handler function.
 *
 * ASYNC-SIGNAL-SAFE: This function can be called directly from signal context.
 *
 * @param signum Signal number (SIGPROF)
 * @param info Signal info structure
 * @param ucontext Platform-specific context
 */
void spprof_signal_handler(int signum, siginfo_t* info, void* ucontext);
#endif

/**
 * Install the signal handler for the given signal.
 *
 * NOT async-signal-safe. Call during profiler startup.
 *
 * @param signum Signal number to handle (typically SIGPROF)
 * @return 0 on success, -1 on error
 */
int signal_handler_install(int signum);

/**
 * Uninstall the signal handler and restore previous handler.
 *
 * @param signum Signal number
 * @return 0 on success, -1 on error
 */
int signal_handler_uninstall(int signum);

/**
 * Start accepting samples (enable the profiler).
 *
 * Call this after the timer is configured but before signals start arriving.
 */
void signal_handler_start(void);

/**
 * Stop accepting samples (pause the profiler).
 *
 * Samples arriving after this call will be ignored.
 */
void signal_handler_stop(void);

/**
 * Enable or disable native (C-stack) frame capture.
 *
 * When enabled, the signal handler will also capture native frames
 * using backtrace() or libunwind.
 *
 * @param enabled 1 to enable, 0 to disable
 */
void signal_handler_set_native(int enabled);

/**
 * Get number of samples successfully captured.
 *
 * @return Number of samples written to ring buffer
 */
uint64_t signal_handler_samples_captured(void);

/**
 * Get number of samples dropped due to buffer overflow.
 *
 * @return Number of samples that couldn't be written
 */
uint64_t signal_handler_samples_dropped(void);

/**
 * Get number of errors encountered in signal handler.
 *
 * @return Number of errors (should normally be 0)
 */
uint64_t signal_handler_errors(void);

/**
 * Get number of samples dropped due to validation failures.
 *
 * On free-threaded Linux builds, speculative frame capture validates
 * pointers before dereferencing. This counter tracks samples that were
 * dropped due to validation failures (cycle detection, invalid pointers,
 * type mismatches).
 *
 * This is a normal condition under free-threading - samples are dropped
 * gracefully rather than risking crashes from reading inconsistent state.
 *
 * @return Number of samples dropped due to validation failures (0 on non-free-threaded)
 */
uint64_t signal_handler_validation_drops(void);

/**
 * Check if we're currently executing in signal handler context.
 *
 * This is used by debug assertions to enforce the invariant that
 * certain operations (like acquiring mutex/rwlock) must never occur
 * from signal context, as this can cause deadlocks.
 *
 * ASYNC-SIGNAL-SAFE: Yes (reads volatile sig_atomic_t).
 *
 * @return 1 if in signal handler, 0 otherwise
 */
int signal_handler_in_context(void);

/*
 * =============================================================================
 * Signal Context Assertions
 * =============================================================================
 *
 * CRITICAL INVARIANT: Lock-based operations must NEVER be called from signal
 * handler context. Violating this invariant causes deadlock:
 *
 *   1. Thread A holds lock L
 *   2. Signal interrupts Thread A
 *   3. Signal handler tries to acquire lock L
 *   4. DEADLOCK: handler waits for lock, but lock holder is blocked in handler
 *
 * Use SPPROF_ASSERT_NOT_IN_SIGNAL() in debug builds to catch violations early.
 *
 * Functions that MUST use this assertion:
 *   - registry_* functions in linux.c (use pthread_rwlock_t)
 *   - Any function that acquires pthread_mutex_t or pthread_rwlock_t
 *   - Any function that calls malloc/free (not async-signal-safe)
 */

#ifdef SPPROF_DEBUG
#include <stdio.h>
#include <stdlib.h>

/**
 * Assert that we are NOT in signal handler context.
 *
 * In debug builds, this aborts with a diagnostic message if called from
 * within a signal handler. In release builds, this is a no-op.
 *
 * Usage:
 *   void my_function_with_lock(void) {
 *       SPPROF_ASSERT_NOT_IN_SIGNAL("my_function_with_lock");
 *       pthread_mutex_lock(&my_lock);
 *       ...
 *   }
 */
#define SPPROF_ASSERT_NOT_IN_SIGNAL(func_name) \
    do { \
        if (signal_handler_in_context()) { \
            fprintf(stderr, \
                "[spprof] FATAL: %s called from signal handler context!\n" \
                "This will cause deadlock. Fix the calling code.\n", \
                (func_name)); \
            abort(); \
        } \
    } while (0)

#else
/* Release build: no-op */
#define SPPROF_ASSERT_NOT_IN_SIGNAL(func_name) ((void)0)
#endif


#ifdef SPPROF_DEBUG
/**
 * Print debug information about signal handler state.
 */
void signal_handler_debug_info(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* SPPROF_SIGNAL_HANDLER_H */

