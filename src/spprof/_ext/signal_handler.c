/**
 * signal_handler.c - Production async-signal-safe signal handler
 *
 * This is the core of the sampling profiler. The signal handler must:
 *   1. Be async-signal-safe (no malloc, no locks, no Python API)
 *   2. Capture the current stack quickly
 *   3. Write to the ring buffer atomically
 *   4. Return as fast as possible
 *
 * Architecture:
 *   Signal (SIGPROF) -> signal_handler() -> capture_stack() -> ringbuffer_write()
 *                            |
 *                            v
 *                    [Lock-free ring buffer]
 *                            |
 *                            v
 *                   [Consumer thread resolves symbols]
 *
 * NOTE: This file is only compiled on POSIX systems (Linux, macOS).
 * Windows has its own implementation in platform/windows.c.
 *
 * FREE-THREADING WARNING (Py_GIL_DISABLED):
 *
 * Signal-based sampling is NOT SAFE for free-threaded Python builds because:
 *
 *   1. FRAME CHAIN INSTABILITY:
 *      In GIL-enabled builds, the GIL ensures frame chains are stable when
 *      a signal interrupts execution. In free-threaded builds, the interrupted
 *      thread could be in the middle of a function call/return, with the
 *      frame->previous pointer in an inconsistent state.
 *
 *   2. NO SYNCHRONIZATION AVAILABLE:
 *      We cannot acquire locks in a signal handler (not async-signal-safe).
 *      We cannot use Python's critical sections (requires Python API).
 *      We cannot ensure the frame chain is consistent.
 *
 *   3. RACE CONDITIONS:
 *      Reading frame->previous while the thread modifies it can result in:
 *      - Reading a half-updated pointer → crash
 *      - Following a stale pointer to freed memory → crash
 *      - Infinite loops if chain becomes circular
 *
 * On Darwin/macOS, we use Mach-based sampling instead (darwin_mach.c) which
 * suspends threads before reading their state. This is safe for free-threading.
 *
 * On Linux with free-threaded Python, signal-based sampling is disabled.
 * Future alternatives could include:
 *   - PEP 669 profiling callbacks (cooperative, at safe points)
 *   - perf_event_open with PEBS (hardware-assisted)
 *   - eBPF-based sampling
 *
 * Copyright (c) 2024 spprof contributors
 * SPDX-License-Identifier: MIT
 */

/* Windows uses its own implementation in platform/windows.c */
#ifndef _WIN32

#define _GNU_SOURCE
#include <Python.h>
#include <signal.h>
#include <stdint.h>
#include <stdatomic.h>
#include <time.h>
#include <string.h>

#ifdef __linux__
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include "ringbuffer.h"
#include "framewalker.h"
#include "unwind.h"

/* Note: Darwin uses Mach-based sampler (darwin_mach.c) instead of signals.
 * This file is still compiled on Darwin for compatibility but the signal
 * handler is not used. */

#ifdef SPPROF_USE_INTERNAL_API
#include "internal/pycore_frame.h"
#include "internal/pycore_tstate.h"
#endif

/*
 * FREE-THREADING SAFETY CHECK
 *
 * On free-threaded builds (Py_GIL_DISABLED) on non-Darwin platforms,
 * signal-based sampling is unsafe. The SPPROF_FREE_THREADING_SAFE macro
 * is defined in pycore_frame.h based on the platform and build configuration.
 *
 * Darwin uses Mach-based sampling (darwin_mach.c) which is safe.
 * This file is still compiled but the handler is effectively disabled.
 */
#ifdef SPPROF_USE_INTERNAL_API
#if !SPPROF_FREE_THREADING_SAFE
    /* Signal handler will return immediately without capturing frames */
    #define SPPROF_SIGNAL_HANDLER_DISABLED 1
#endif
#endif

/*
 * =============================================================================
 * Globals (must be accessible from signal handler)
 * =============================================================================
 */

/* Ring buffer - set by module.c */
extern RingBuffer* g_ringbuffer;

/* Profiler state */
static volatile sig_atomic_t g_profiler_active = 0;
static volatile sig_atomic_t g_in_handler = 0;  /* Reentrancy guard */

/* Statistics (updated atomically) */
static _Atomic uint64_t g_samples_captured = 0;
static _Atomic uint64_t g_samples_dropped = 0;
static _Atomic uint64_t g_handler_errors = 0;
static _Atomic uint64_t g_walk_depth_sum = 0;  /* Debug: sum of all walk depths */

/* Configuration */
static int g_capture_native = 0;
static int g_skip_frames = 2;  /* Skip signal handler frames */

/*
 * =============================================================================
 * Free-Threading Speculative Capture State (Linux only)
 * =============================================================================
 *
 * These globals are used by the speculative frame capture functions for
 * free-threaded Python builds on Linux. They are:
 *   - Initialized once at module load (with GIL held)
 *   - Read-only during signal handling (async-signal-safe)
 *   - Never modified after initialization
 */
#if SPPROF_FREE_THREADED && defined(__linux__)

/* Cached PyCode_Type pointer for async-signal-safe type checking */
PyTypeObject *_spprof_cached_code_type = NULL;

/* Initialization flag */
int _spprof_speculative_initialized = 0;

/* Counter for samples dropped due to validation failures */
_Atomic uint64_t _spprof_samples_dropped_validation = 0;

#endif /* SPPROF_FREE_THREADED && __linux__ */

/*
 * =============================================================================
 * Async-Signal-Safe Utilities
 * =============================================================================
 */

/**
 * Get monotonic timestamp - ASYNC-SIGNAL-SAFE
 *
 * clock_gettime with CLOCK_MONOTONIC is async-signal-safe per POSIX.
 */
static inline uint64_t get_timestamp_ns_unsafe(void) {
#ifdef __linux__
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#elif defined(__APPLE__)
    /* Use mach_absolute_time on macOS */
    return clock_gettime_nsec_np(CLOCK_MONOTONIC);
#elif defined(_WIN32)
    /* Windows - would need different approach */
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return (uint64_t)count.QuadPart * 1000000000ULL / (uint64_t)freq.QuadPart;
#else
    return 0;
#endif
}

/**
 * Get thread ID - ASYNC-SIGNAL-SAFE
 */
static inline uint64_t get_thread_id_unsafe(void) {
#ifdef __linux__
    return (uint64_t)syscall(SYS_gettid);
#elif defined(__APPLE__)
    uint64_t tid;
    pthread_threadid_np(NULL, &tid);
    return tid;
#elif defined(_WIN32)
    return (uint64_t)GetCurrentThreadId();
#else
    return 0;
#endif
}

/*
 * =============================================================================
 * Stack Capture (ASYNC-SIGNAL-SAFE)
 * =============================================================================
 */

/**
 * Capture Python stack frames - ASYNC-SIGNAL-SAFE
 *
 * This function reads Python's internal frame structures directly
 * without calling any Python C API functions.
 *
 * On free-threaded Linux builds, uses speculative capture with validation.
 */
static inline int
capture_python_stack_unsafe(uintptr_t* frames, int max_depth) {
#ifdef SPPROF_USE_INTERNAL_API
    #if SPPROF_FREE_THREADED && defined(__linux__)
        /* Free-threaded Linux: Use speculative capture with validation */
        return _spprof_capture_frames_speculative(frames, max_depth);
    #else
        /* GIL-enabled or Darwin (uses Mach sampler): Use direct capture */
        return _spprof_capture_frames_unsafe(frames, max_depth);
    #endif
#else
    /* Fallback: use framewalker (may not be fully signal-safe) */
    return framewalker_capture_raw(frames, max_depth);
#endif
}

/**
 * Capture Python stack frames with instruction pointers - ASYNC-SIGNAL-SAFE
 *
 * This variant also captures instruction pointers for accurate line numbers.
 *
 * On free-threaded Linux builds, uses speculative capture with validation.
 */
static inline int
capture_python_stack_with_instr_unsafe(uintptr_t* frames, uintptr_t* instr_ptrs, int max_depth) {
#ifdef SPPROF_USE_INTERNAL_API
    #if SPPROF_FREE_THREADED && defined(__linux__)
        /* Free-threaded Linux: Use speculative capture with validation */
        return _spprof_capture_frames_with_instr_speculative(frames, instr_ptrs, max_depth);
    #else
        /* GIL-enabled or Darwin (uses Mach sampler): Use direct capture */
        return _spprof_capture_frames_with_instr_unsafe(frames, instr_ptrs, max_depth);
    #endif
#else
    /* Fallback: capture frames only, no instruction pointers */
    int depth = framewalker_capture_raw(frames, max_depth);
    /* Zero out instruction pointers - resolver will use first line */
    for (int i = 0; i < depth; i++) {
        instr_ptrs[i] = 0;
    }
    return depth;
#endif
}

/**
 * Capture native (C) stack frames - ASYNC-SIGNAL-SAFE
 *
 * Uses backtrace() which is generally async-signal-safe on Linux/macOS.
 */
static inline int
capture_native_stack_unsafe(NativeStack* stack, int skip) {
    if (!g_capture_native) {
        return 0;
    }
    return unwind_capture(stack, skip);
}

/*
 * =============================================================================
 * Signal Handler
 * =============================================================================
 */

/**
 * Production signal handler - ASYNC-SIGNAL-SAFE
 *
 * Called by the kernel when SIGPROF timer fires. Must be extremely fast
 * and safe - any bug here can crash the entire process.
 *
 * Safety checklist:
 *   ✓ No malloc/free
 *   ✓ No printf/fprintf
 *   ✓ No Python C API calls
 *   ✓ No mutex locks
 *   ✓ Reentrancy protected
 *   ✓ Uses only stack-allocated storage
 *
 * FREE-THREADING:
 *   On free-threaded builds (Py_GIL_DISABLED) without Darwin/Mach support,
 *   this handler returns immediately without capturing frames to avoid
 *   unsafe frame chain walking. See module.c for the user-facing error.
 */
void spprof_signal_handler(int signum, siginfo_t* info, void* ucontext) {
    (void)signum;
    (void)info;
    (void)ucontext;
    
#ifdef SPPROF_SIGNAL_HANDLER_DISABLED
    /*
     * FREE-THREADING SAFETY:
     * Signal-based sampling is disabled on this configuration because
     * walking the frame chain without the GIL is unsafe. The handler
     * simply returns without capturing any samples.
     *
     * This should never be reached if module.c properly blocks startup,
     * but serves as a defense-in-depth measure.
     */
    return;
#endif
    
    /* Quick exit if profiler not active */
    if (!g_profiler_active || g_ringbuffer == NULL) {
        return;
    }
    
    /* Reentrancy guard - prevent recursive signals */
    if (g_in_handler) {
        return;
    }
    g_in_handler = 1;
    
    /* Get timestamp immediately (most accurate timing) */
    uint64_t timestamp = get_timestamp_ns_unsafe();
    
    /* Get thread ID */
    uint64_t thread_id = get_thread_id_unsafe();
    
    /* Stack-allocated sample buffer */
    RawSample sample;
    sample.timestamp = timestamp;
    sample.thread_id = thread_id;
    sample.native_depth = 0;
    
    /* Capture Python frames with instruction pointers for accurate line numbers */
    sample.depth = capture_python_stack_with_instr_unsafe(
        sample.frames,
        sample.instr_ptrs,
        SPPROF_MAX_STACK_DEPTH - g_skip_frames
    );
    
    /* Optional: Capture native frames and merge */
    if (g_capture_native && sample.depth < SPPROF_MAX_STACK_DEPTH) {
        NativeStack native_stack;
        int native_depth = capture_native_stack_unsafe(&native_stack, g_skip_frames);
        
        /* Append native frames after Python frames (simplified - could interleave) */
        for (int i = 0; i < native_depth && sample.depth < SPPROF_MAX_STACK_DEPTH; i++) {
            /* Mark as native by setting high bit */
            sample.frames[sample.depth++] = native_stack.frames[i].ip | 0x8000000000000000ULL;
        }
    }
    
    /* Write to ring buffer (lock-free, async-signal-safe) */
    if (sample.depth > 0) {
        if (ringbuffer_write(g_ringbuffer, &sample)) {
            atomic_fetch_add_explicit(&g_samples_captured, 1, memory_order_relaxed);
        } else {
            atomic_fetch_add_explicit(&g_samples_dropped, 1, memory_order_relaxed);
        }
    }
    
    /* Clear reentrancy guard */
    g_in_handler = 0;
}

/*
 * =============================================================================
 * Handler Management
 * =============================================================================
 */

static struct sigaction g_old_action;
static int g_handler_installed = 0;

/**
 * Install the signal handler
 *
 * NOT async-signal-safe - call during profiler startup only.
 */
int signal_handler_install(int signum) {
    if (g_handler_installed) {
        return 0;  /* Already installed */
    }
    
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    
    sa.sa_sigaction = spprof_signal_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    
    /* Block SIGPROF during handler execution to prevent reentrancy */
    sigaddset(&sa.sa_mask, signum);
    
    if (sigaction(signum, &sa, &g_old_action) < 0) {
        return -1;
    }
    
    g_handler_installed = 1;
    return 0;
}

/**
 * Uninstall the signal handler
 * 
 * We set SIG_IGN first to drop any pending signals, then restore
 * the original handler.
 */
int signal_handler_uninstall(int signum) {
    if (!g_handler_installed) {
        return 0;
    }
    
    /* First, ignore the signal to drop any pending deliveries */
    struct sigaction sa_ignore;
    memset(&sa_ignore, 0, sizeof(sa_ignore));
    sa_ignore.sa_handler = SIG_IGN;
    sigemptyset(&sa_ignore.sa_mask);
    sigaction(signum, &sa_ignore, NULL);
    
    /* Brief pause to let kernel discard pending signals */
    struct timespec ts = {0, 1000000};  /* 1ms */
    nanosleep(&ts, NULL);
    
    /* Now restore original handler (if it wasn't SIG_DFL which would kill us) */
    if (g_old_action.sa_handler != SIG_DFL && 
        !(g_old_action.sa_flags & SA_SIGINFO && g_old_action.sa_sigaction == NULL)) {
        sigaction(signum, &g_old_action, NULL);
    }
    /* Otherwise leave as SIG_IGN which is safe */
    
    g_handler_installed = 0;
    return 0;
}

/**
 * Start accepting samples
 */
void signal_handler_start(void) {
    /* Reset statistics */
    atomic_store(&g_samples_captured, 0);
    atomic_store(&g_samples_dropped, 0);
    atomic_store(&g_handler_errors, 0);
    
    /* Enable sample capture */
    g_profiler_active = 1;
}

/**
 * Stop accepting samples
 */
void signal_handler_stop(void) {
    g_profiler_active = 0;
}

/**
 * Configure native frame capture
 */
void signal_handler_set_native(int enabled) {
    g_capture_native = enabled;
}

/*
 * =============================================================================
 * Statistics
 * =============================================================================
 */

uint64_t signal_handler_samples_captured(void) {
    return atomic_load(&g_samples_captured);
}

uint64_t signal_handler_samples_dropped(void) {
    return atomic_load(&g_samples_dropped);
}

uint64_t signal_handler_errors(void) {
    return atomic_load(&g_handler_errors);
}

/**
 * Get number of samples dropped due to validation failures (free-threading).
 *
 * This counter is only incremented on free-threaded Linux builds when
 * speculative frame capture detects validation failures (cycle detection,
 * invalid pointers, etc.).
 *
 * @return Number of samples dropped due to validation failures
 */
uint64_t signal_handler_validation_drops(void) {
#if SPPROF_FREE_THREADED && defined(__linux__)
    return atomic_load(&_spprof_samples_dropped_validation);
#else
    return 0;
#endif
}

/**
 * Check if we're currently executing in signal handler context.
 *
 * This is used by debug assertions to enforce the invariant that
 * certain operations (like acquiring locks) must never occur from
 * signal context.
 *
 * ASYNC-SIGNAL-SAFE: Yes (reads volatile sig_atomic_t).
 *
 * @return 1 if in signal handler, 0 otherwise
 */
int signal_handler_in_context(void) {
    return g_in_handler != 0;
}

/*
 * =============================================================================
 * Debug Support
 * =============================================================================
 */

#ifdef SPPROF_DEBUG

#include <stdio.h>

void signal_handler_debug_info(void) {
    fprintf(stderr, "[spprof] Signal Handler Status:\n");
    fprintf(stderr, "  Installed: %d\n", g_handler_installed);
    fprintf(stderr, "  Active: %d\n", (int)g_profiler_active);
    fprintf(stderr, "  Native capture: %d\n", g_capture_native);
    fprintf(stderr, "  Samples captured: %llu\n", 
            (unsigned long long)signal_handler_samples_captured());
    fprintf(stderr, "  Samples dropped: %llu\n",
            (unsigned long long)signal_handler_samples_dropped());
    fprintf(stderr, "  Handler errors: %llu\n",
            (unsigned long long)signal_handler_errors());
    fprintf(stderr, "  Ring buffer: %p\n", (void*)g_ringbuffer);
}

#endif /* SPPROF_DEBUG */

#endif /* !_WIN32 */

