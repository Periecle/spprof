/**
 * signal_handler.h - Production async-signal-safe signal handler interface
 *
 * This module provides the signal handling infrastructure for sampling.
 * The actual signal handler is fully async-signal-safe.
 *
 * Copyright (c) 2024 spprof contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef SPPROF_SIGNAL_HANDLER_H
#define SPPROF_SIGNAL_HANDLER_H

#include <signal.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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

