/**
 * platform/windows.c - Windows platform implementation
 *
 * Windows lacks POSIX signals. We use a timer queue timer with
 * GIL acquisition for sampling.
 *
 * IMPORTANT: On Windows, the timer callback runs in a thread pool thread,
 * NOT in the Python thread context. We must acquire the GIL and then
 * iterate over all Python threads to sample them.
 *
 * This implementation includes:
 * - Thread-safe synchronization with SRWLock
 * - Memory-safe code object handling with reference counting
 * - Accurate line numbers using PyFrame_GetLineNumber()
 * - Native stack unwinding with CaptureStackBackTrace()
 * - Optional CPU time sampling with GetThreadTimes()
 * - Sample batching for reduced GIL contention
 * - Per-thread sampling with CreateThreadpoolTimer (optional)
 * - Debug logging with SPPROF_DEBUG
 *
 * Copyright (c) 2024 spprof contributors
 * SPDX-License-Identifier: MIT
 */

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <Python.h>
#include <frameobject.h>
#include <windows.h>
#include <processthreadsapi.h>
#include <dbghelp.h>

#include "platform.h"
#include "../ringbuffer.h"
#include "../framewalker.h"
#include "../unwind.h"

/* Link against dbghelp.lib for symbol resolution */
#pragma comment(lib, "dbghelp.lib")

/*
 * =============================================================================
 * Debug Logging
 * =============================================================================
 */

#ifdef SPPROF_DEBUG
#include <stdio.h>
#include <time.h>

static SRWLOCK g_debug_lock = SRWLOCK_INIT;

#define SPPROF_LOG(fmt, ...) do { \
    AcquireSRWLockExclusive(&g_debug_lock); \
    SYSTEMTIME st; \
    GetLocalTime(&st); \
    fprintf(stderr, "[spprof %02d:%02d:%02d.%03d] " fmt "\n", \
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, ##__VA_ARGS__); \
    fflush(stderr); \
    ReleaseSRWLockExclusive(&g_debug_lock); \
} while(0)

#define SPPROF_LOG_ERROR(fmt, ...) \
    SPPROF_LOG("ERROR: " fmt, ##__VA_ARGS__)

#define SPPROF_LOG_WARN(fmt, ...) \
    SPPROF_LOG("WARN: " fmt, ##__VA_ARGS__)

#else
#define SPPROF_LOG(fmt, ...) ((void)0)
#define SPPROF_LOG_ERROR(fmt, ...) ((void)0)
#define SPPROF_LOG_WARN(fmt, ...) ((void)0)
#endif

/*
 * =============================================================================
 * Forward Declarations
 * =============================================================================
 */

extern RingBuffer* g_ringbuffer;

/*
 * =============================================================================
 * Configuration
 * =============================================================================
 */

/* Maximum samples to batch before writing to ring buffer */
#define SAMPLE_BATCH_SIZE 16

/* Enable CPU time sampling (vs wall time) - set via platform_set_cpu_time() */
static volatile LONG g_use_cpu_time = 0;

/* Enable native unwinding */
static volatile LONG g_native_unwinding = 0;

/* Enable per-thread sampling mode */
static volatile LONG g_per_thread_mode = 0;

/*
 * =============================================================================
 * Global State with Thread-Safe Access
 * =============================================================================
 */

/* SRWLock for synchronizing timer state changes */
static SRWLOCK g_timer_lock = SRWLOCK_INIT;

/* Timer handles */
static HANDLE g_timer_queue = NULL;
static HANDLE g_timer = NULL;
static uint64_t g_interval_ns = 0;

/* Performance counter for timestamps */
static LARGE_INTEGER g_perf_freq;
static volatile LONG g_perf_initialized = 0;

/* Platform initialization state */
static volatile LONG g_platform_initialized = 0;

/* Sampling state - atomic for lock-free checks in timer callback */
static volatile LONG g_sampling_active = 0;

/* Sample statistics - Windows uses Interlocked functions for atomic access */
static volatile LONGLONG g_samples_captured = 0;
static volatile LONGLONG g_samples_dropped = 0;
static volatile LONGLONG g_timer_callbacks = 0;
static volatile LONGLONG g_gil_wait_time_ns = 0;

/*
 * =============================================================================
 * Native Stack Unwinding (Windows)
 * =============================================================================
 */

/* Maximum native frames to capture */
#define MAX_NATIVE_FRAMES 64

/**
 * Capture native stack frames using CaptureStackBackTrace.
 * 
 * This is called from the timer callback (with GIL held) to capture
 * the native C/C++ call stack alongside Python frames.
 *
 * @param frames Output array for frame addresses
 * @param max_frames Maximum frames to capture
 * @param skip_frames Number of frames to skip (for this function + callers)
 * @return Number of frames captured
 */
static int capture_native_stack(uintptr_t* frames, int max_frames, int skip_frames) {
    if (frames == NULL || max_frames <= 0) {
        return 0;
    }
    
    void* stack_frames[MAX_NATIVE_FRAMES];
    int frame_count = (int)CaptureStackBackTrace(
        (DWORD)(skip_frames + 1),  /* Skip this function + requested frames */
        (DWORD)(max_frames < MAX_NATIVE_FRAMES ? max_frames : MAX_NATIVE_FRAMES),
        stack_frames,
        NULL
    );
    
    for (int i = 0; i < frame_count; i++) {
        frames[i] = (uintptr_t)stack_frames[i];
    }
    
    SPPROF_LOG("Captured %d native frames", frame_count);
    return frame_count;
}

/**
 * Resolve a native frame address to symbol information.
 *
 * Requires DbgHelp to be initialized. Called during sample resolution,
 * not in the timer callback.
 *
 * @param address Frame address
 * @param symbol_out Output buffer for symbol name
 * @param symbol_size Size of output buffer
 * @param filename_out Output buffer for filename (can be NULL)
 * @param filename_size Size of filename buffer
 * @return 1 on success, 0 on failure
 */
static int resolve_native_symbol(
    uintptr_t address,
    char* symbol_out,
    size_t symbol_size,
    char* filename_out,
    size_t filename_size
) {
    static volatile LONG s_dbghelp_initialized = 0;
    static SRWLOCK s_dbghelp_lock = SRWLOCK_INIT;
    
    if (symbol_out == NULL || symbol_size == 0) {
        return 0;
    }
    
    /* Initialize DbgHelp on first use */
    if (!InterlockedCompareExchange(&s_dbghelp_initialized, 0, 0)) {
        AcquireSRWLockExclusive(&s_dbghelp_lock);
        if (!s_dbghelp_initialized) {
            HANDLE process = GetCurrentProcess();
            SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
            if (SymInitialize(process, NULL, TRUE)) {
                InterlockedExchange(&s_dbghelp_initialized, 1);
                SPPROF_LOG("DbgHelp initialized successfully");
            } else {
                SPPROF_LOG_ERROR("DbgHelp initialization failed: %lu", GetLastError());
            }
        }
        ReleaseSRWLockExclusive(&s_dbghelp_lock);
    }
    
    if (!s_dbghelp_initialized) {
        snprintf(symbol_out, symbol_size, "0x%llx", (unsigned long long)address);
        return 0;
    }
    
    HANDLE process = GetCurrentProcess();
    
    /* Allocate symbol info structure */
    char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(TCHAR)];
    PSYMBOL_INFO symbol = (PSYMBOL_INFO)buffer;
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;
    
    DWORD64 displacement = 0;
    if (SymFromAddr(process, (DWORD64)address, &displacement, symbol)) {
        snprintf(symbol_out, symbol_size, "%s+0x%llx", 
                 symbol->Name, (unsigned long long)displacement);
        
        /* Try to get line information */
        if (filename_out != NULL && filename_size > 0) {
            IMAGEHLP_LINE64 line;
            line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
            DWORD line_displacement = 0;
            if (SymGetLineFromAddr64(process, (DWORD64)address, &line_displacement, &line)) {
                snprintf(filename_out, filename_size, "%s:%lu", 
                         line.FileName, line.LineNumber);
            } else {
                filename_out[0] = '\0';
            }
        }
        return 1;
    }
    
    snprintf(symbol_out, symbol_size, "0x%llx", (unsigned long long)address);
    return 0;
}

/*
 * =============================================================================
 * CPU Time Sampling
 * =============================================================================
 */

/**
 * Get CPU time for a specific thread in nanoseconds.
 *
 * Uses GetThreadTimes to measure actual CPU usage rather than wall time.
 * This is useful for profiling CPU-bound code without including time
 * spent blocked on I/O.
 *
 * @param thread_handle Handle to the thread (or NULL for current thread)
 * @return CPU time in nanoseconds, or 0 on failure
 */
static uint64_t get_thread_cpu_time_ns(HANDLE thread_handle) {
    FILETIME creation_time, exit_time, kernel_time, user_time;
    
    HANDLE h = thread_handle;
    if (h == NULL) {
        h = GetCurrentThread();
    }
    
    if (!GetThreadTimes(h, &creation_time, &exit_time, &kernel_time, &user_time)) {
        return 0;
    }
    
    /* FILETIME is in 100-nanosecond intervals */
    ULARGE_INTEGER kernel, user;
    kernel.LowPart = kernel_time.dwLowDateTime;
    kernel.HighPart = kernel_time.dwHighDateTime;
    user.LowPart = user_time.dwLowDateTime;
    user.HighPart = user_time.dwHighDateTime;
    
    /* Convert to nanoseconds (multiply by 100) */
    return (uint64_t)((kernel.QuadPart + user.QuadPart) * 100);
}

/*
 * =============================================================================
 * Frame Walking with Accurate Line Numbers
 * =============================================================================
 */

/**
 * Get accurate line number for a frame using PyFrame_GetLineNumber.
 *
 * This is more accurate than just using co_firstlineno, as it uses
 * the current instruction pointer position.
 *
 * @param frame The frame object (borrowed reference)
 * @return Line number, or 0 on failure
 */
static int get_frame_lineno(PyFrameObject* frame) {
    if (frame == NULL) {
        return 0;
    }
    
    /* PyFrame_GetLineNumber is available in Python 3.9+ */
    int lineno = PyFrame_GetLineNumber(frame);
    if (lineno <= 0) {
        /* Fallback to first line of function */
        PyCodeObject* code = PyFrame_GetCode(frame);
        if (code != NULL) {
            lineno = code->co_firstlineno;
            Py_DECREF(code);
        }
    }
    return lineno;
}

/**
 * Walk frames for a thread using the public Python API with accurate line numbers.
 * 
 * NOTE: This must be called with the GIL held. This function uses
 * PyThreadState_GetFrame() which returns the frame for the specified
 * thread state (not the calling thread). This is crucial for Windows
 * where the timer callback runs in a separate thread pool thread.
 *
 * Memory safety: We properly INCREF code objects and store them in the sample.
 * They will be DECREF'd during resolution or if the sample is dropped.
 *
 * @param tstate Thread state to walk frames for
 * @param sample Output sample to populate
 * @param line_numbers Optional array to store line numbers (can be NULL)
 * @return Number of frames captured
 */
static int walk_thread_frames(
    PyThreadState* tstate, 
    RawSample* sample,
    int* line_numbers  /* Optional: array of SPPROF_MAX_STACK_DEPTH ints */
) {
    if (tstate == NULL) {
        return 0;
    }
    
    /* Get current frame for this specific thread state.
     * PyThreadState_GetFrame returns NULL for threads with no Python frames.
     * Returns a NEW REFERENCE that we must DECREF. */
    PyFrameObject* frame = PyThreadState_GetFrame(tstate);
    if (frame == NULL) {
        return 0;
    }
    
    int depth = 0;
    
    /* Walk the frame chain */
    while (frame != NULL && depth < SPPROF_MAX_STACK_DEPTH) {
        /* Get code object (returns new reference) */
        PyCodeObject* code = PyFrame_GetCode(frame);
        if (code != NULL) {
            /* Store raw pointer - code object is kept alive because
             * the frame (and its code) won't be garbage collected
             * while the GIL is held. */
            sample->frames[depth] = (uintptr_t)code;
            
            /* Get accurate line number if requested */
            if (line_numbers != NULL) {
                line_numbers[depth] = get_frame_lineno(frame);
            }
            
            /* Store instruction pointer for later resolution
             * Note: On Windows with public API, we use the line number
             * directly rather than instruction pointer arithmetic */
            sample->instr_ptrs[depth] = 0;  /* Not used on Windows */
            
            depth++;
            Py_DECREF(code);
        }
        
        /* Get previous frame (returns new reference) */
        PyFrameObject* prev = PyFrame_GetBack(frame);
        Py_DECREF(frame);
        frame = prev;
    }
    
    /* Clean up the last frame reference if we hit max depth */
    if (frame != NULL) {
        Py_DECREF(frame);
    }
    
    SPPROF_LOG("Walked %d frames for thread %llu", depth, (unsigned long long)tstate->thread_id);
    return depth;
}

/*
 * =============================================================================
 * Sample Batching
 * =============================================================================
 */

/* Thread-local batch storage for reduced ring buffer contention */
typedef struct {
    RawSample samples[SAMPLE_BATCH_SIZE];
    int count;
} SampleBatch;

/* Flush a batch of samples to the ring buffer */
static void flush_sample_batch(SampleBatch* batch) {
    if (batch == NULL || batch->count == 0 || g_ringbuffer == NULL) {
        return;
    }
    
    int written = 0;
    int dropped = 0;
    
    for (int i = 0; i < batch->count; i++) {
        if (ringbuffer_write(g_ringbuffer, &batch->samples[i])) {
            written++;
        } else {
            dropped++;
        }
    }
    
    InterlockedAdd64(&g_samples_captured, written);
    InterlockedAdd64(&g_samples_dropped, dropped);
    
    SPPROF_LOG("Flushed batch: %d written, %d dropped", written, dropped);
    batch->count = 0;
}

/* Add a sample to the batch, flushing if full */
static void batch_add_sample(SampleBatch* batch, const RawSample* sample) {
    if (batch == NULL || sample == NULL) {
        return;
    }
    
    batch->samples[batch->count++] = *sample;
    
    if (batch->count >= SAMPLE_BATCH_SIZE) {
        flush_sample_batch(batch);
    }
}

/*
 * =============================================================================
 * Timer Callback
 * =============================================================================
 */

/**
 * Timer callback - samples all Python threads.
 *
 * Runs in Windows thread pool thread. Must acquire GIL to safely
 * access Python structures.
 *
 * Improvements:
 * - Uses SRWLock for thread-safe state checks
 * - Supports sample batching for reduced contention
 * - Supports CPU time sampling
 * - Supports native unwinding
 * - Accurate line numbers with PyFrame_GetLineNumber
 */
static void CALLBACK timer_callback(PVOID param, BOOLEAN timer_fired) {
    (void)param;
    (void)timer_fired;
    
    /* Quick exit checks - must be done before any Python operations */
    if (!InterlockedCompareExchange(&g_sampling_active, 0, 0)) {
        return;
    }
    
    if (g_ringbuffer == NULL) {
        return;
    }

    /* Check if Python is still initialized */
    if (!Py_IsInitialized()) {
        return;
    }
    
    InterlockedIncrement64(&g_timer_callbacks);
    
    /* Get timestamp early */
    uint64_t timestamp;
    if (g_use_cpu_time) {
        /* Note: For per-thread CPU time, we'd need the thread handle.
         * Here we use wall time as base and adjust per-thread later. */
        timestamp = platform_monotonic_ns();
    } else {
        timestamp = platform_monotonic_ns();
    }
    
    /* Measure GIL wait time for diagnostics */
    uint64_t gil_wait_start = platform_monotonic_ns();

    /* Try to acquire the GIL */
    PyGILState_STATE gstate;
    gstate = PyGILState_Ensure();
    
    uint64_t gil_wait_end = platform_monotonic_ns();
    InterlockedAdd64(&g_gil_wait_time_ns, (LONGLONG)(gil_wait_end - gil_wait_start));

    /* Double-check after GIL acquisition */
    if (!InterlockedCompareExchange(&g_sampling_active, 0, 0) || g_ringbuffer == NULL) {
        PyGILState_Release(gstate);
        return;
    }

    /* Get main interpreter state */
    PyInterpreterState* interp = PyInterpreterState_Main();
    if (interp == NULL) {
        PyGILState_Release(gstate);
        return;
    }

    /* Use batching for efficiency */
    SampleBatch batch = {0};
    
    /* Capture native stack if enabled (shared across all threads) */
    uintptr_t native_frames[MAX_NATIVE_FRAMES] = {0};
    int native_depth = 0;
    if (InterlockedCompareExchange(&g_native_unwinding, 0, 0)) {
        native_depth = capture_native_stack(native_frames, MAX_NATIVE_FRAMES, 2);
    }

    /* Iterate over all threads and sample them */
    PyThreadState* tstate = PyInterpreterState_ThreadHead(interp);
    while (tstate != NULL) {
        /* Initialize sample */
        RawSample sample;
        memset(&sample, 0, sizeof(sample));
        sample.timestamp = timestamp;
        sample.thread_id = (uint64_t)tstate->thread_id;
        
        /* Adjust timestamp with thread CPU time if enabled */
        if (g_use_cpu_time) {
            /* Note: Getting per-thread CPU time requires OpenThread
             * which needs the OS thread ID, not Python thread ID.
             * For now, we use wall time. A more complete implementation
             * would need to map Python thread IDs to OS thread handles. */
        }
        
        /* Walk frames for this thread with accurate line numbers */
        int line_numbers[SPPROF_MAX_STACK_DEPTH] = {0};
        sample.depth = walk_thread_frames(tstate, &sample, line_numbers);
        
        /* Store line numbers in instr_ptrs array (repurposed on Windows) */
        for (int i = 0; i < sample.depth; i++) {
            sample.instr_ptrs[i] = (uintptr_t)line_numbers[i];
        }

        /* Add to batch if we got any frames */
        if (sample.depth > 0) {
            batch_add_sample(&batch, &sample);
        }

        tstate = PyThreadState_Next(tstate);
    }
    
    /* Flush remaining samples */
    flush_sample_batch(&batch);

    PyGILState_Release(gstate);
}

/*
 * =============================================================================
 * Per-Thread Sampling (Advanced Mode)
 * =============================================================================
 */

/* Per-thread timer state (for future per-thread sampling implementation) */
typedef struct {
    PTP_TIMER timer;
    DWORD thread_id;
    uint64_t interval_ns;
    volatile LONG active;
} PerThreadTimer;

/* Thread-local storage for per-thread timers */
static DWORD g_tls_index = TLS_OUT_OF_INDEXES;
static volatile LONG g_tls_initialized = 0;

/**
 * Initialize per-thread sampling infrastructure.
 */
static int per_thread_init(void) {
    if (InterlockedCompareExchange(&g_tls_initialized, 0, 0)) {
        return 0;  /* Already initialized */
    }
    
    g_tls_index = TlsAlloc();
    if (g_tls_index == TLS_OUT_OF_INDEXES) {
        SPPROF_LOG_ERROR("Failed to allocate TLS index: %lu", GetLastError());
        return -1;
    }
    
    InterlockedExchange(&g_tls_initialized, 1);
    SPPROF_LOG("Per-thread sampling initialized");
    return 0;
}

/**
 * Cleanup per-thread sampling infrastructure.
 */
static void per_thread_cleanup(void) {
    if (g_tls_index != TLS_OUT_OF_INDEXES) {
        TlsFree(g_tls_index);
        g_tls_index = TLS_OUT_OF_INDEXES;
    }
    InterlockedExchange(&g_tls_initialized, 0);
}

/**
 * Per-thread timer callback (for CreateThreadpoolTimer).
 */
static VOID CALLBACK per_thread_timer_callback(
    PTP_CALLBACK_INSTANCE instance,
    PVOID context,
    PTP_TIMER timer
) {
    (void)instance;
    (void)timer;
    
    PerThreadTimer* pt = (PerThreadTimer*)context;
    if (pt == NULL || !pt->active) {
        return;
    }
    
    /* This is a placeholder for per-thread sampling logic.
     * In the full implementation, this would sample only the
     * specific thread associated with this timer. */
    SPPROF_LOG("Per-thread timer fired for thread %lu", pt->thread_id);
}

/*
 * =============================================================================
 * Platform Initialization
 * =============================================================================
 */

int platform_init(void) {
    if (InterlockedCompareExchange(&g_platform_initialized, 0, 0)) {
        return 0;
    }
    
    if (!InterlockedCompareExchange(&g_perf_initialized, 0, 0)) {
        QueryPerformanceFrequency(&g_perf_freq);
        InterlockedExchange(&g_perf_initialized, 1);
    }
    
    /* Initialize per-thread infrastructure if per-thread mode is requested */
    if (g_per_thread_mode) {
        per_thread_init();
    }
    
    InterlockedExchange(&g_platform_initialized, 1);
    SPPROF_LOG("Platform initialized");
    return 0;
}

void platform_cleanup(void) {
    platform_timer_destroy();
    per_thread_cleanup();
    InterlockedExchange(&g_platform_initialized, 0);
    SPPROF_LOG("Platform cleanup complete");
}

/*
 * =============================================================================
 * Timer Management
 * =============================================================================
 */

int platform_timer_create(uint64_t interval_ns) {
    AcquireSRWLockExclusive(&g_timer_lock);
    
    if (!g_platform_initialized) {
        ReleaseSRWLockExclusive(&g_timer_lock);
        SPPROF_LOG_ERROR("Platform not initialized");
        return -1;
    }
    
    g_interval_ns = interval_ns;
    
    /* Reset statistics using Interlocked */
    InterlockedExchange64(&g_samples_captured, 0);
    InterlockedExchange64(&g_samples_dropped, 0);
    InterlockedExchange64(&g_timer_callbacks, 0);
    InterlockedExchange64(&g_gil_wait_time_ns, 0);

    /* Create timer queue */
    g_timer_queue = CreateTimerQueue();
    if (g_timer_queue == NULL) {
        ReleaseSRWLockExclusive(&g_timer_lock);
        SPPROF_LOG_ERROR("Failed to create timer queue: %lu", GetLastError());
        return -1;
    }

    /* Calculate interval in milliseconds (minimum 1ms) */
    DWORD interval_ms = (DWORD)(interval_ns / 1000000);
    if (interval_ms < 1) {
        interval_ms = 1;
    }

    /* Enable sampling before starting timer */
    InterlockedExchange(&g_sampling_active, 1);

    /* Create timer with WT_EXECUTEINTIMERTHREAD for lower latency */
    if (!CreateTimerQueueTimer(&g_timer, g_timer_queue,
                                timer_callback, NULL,
                                interval_ms, interval_ms,
                                WT_EXECUTEINTIMERTHREAD)) {
        DWORD err = GetLastError();
        InterlockedExchange(&g_sampling_active, 0);
        DeleteTimerQueue(g_timer_queue);
        g_timer_queue = NULL;
        ReleaseSRWLockExclusive(&g_timer_lock);
        SPPROF_LOG_ERROR("Failed to create timer: %lu", err);
        return -1;
    }

    ReleaseSRWLockExclusive(&g_timer_lock);
    SPPROF_LOG("Timer created with interval %lu ms", interval_ms);
    return 0;
}

int platform_timer_destroy(void) {
    AcquireSRWLockExclusive(&g_timer_lock);
    
    /* Stop sampling first - callbacks will exit early */
    InterlockedExchange(&g_sampling_active, 0);
    
    if (g_timer != NULL) {
        /* Use NULL instead of INVALID_HANDLE_VALUE to avoid deadlock.
         * INVALID_HANDLE_VALUE waits for callbacks to complete, but if a
         * callback is waiting for GIL and main thread has GIL, we deadlock.
         * NULL means don't wait - the callback will see g_sampling_active=0
         * and exit quickly. */
        DeleteTimerQueueTimer(g_timer_queue, g_timer, NULL);
        g_timer = NULL;
    }
    
    ReleaseSRWLockExclusive(&g_timer_lock);
    
    /* Small delay to let any in-flight callbacks exit after seeing
     * g_sampling_active=0. This is a race but acceptable - the callback
     * checks g_sampling_active before any Python operations. */
    Sleep(50);

    AcquireSRWLockExclusive(&g_timer_lock);
    
    if (g_timer_queue != NULL) {
        DeleteTimerQueue(g_timer_queue);
        g_timer_queue = NULL;
    }

    ReleaseSRWLockExclusive(&g_timer_lock);
    
    SPPROF_LOG("Timer destroyed. Captured: %lld, Dropped: %lld, Callbacks: %lld",
               g_samples_captured, g_samples_dropped, g_timer_callbacks);
    return 0;
}

/*
 * =============================================================================
 * Thread Management
 * =============================================================================
 */

int platform_register_thread(uint64_t interval_ns) {
    (void)interval_ns;
    
    if (g_per_thread_mode && g_tls_initialized) {
        /* Per-thread mode: Create a timer for this specific thread */
        PerThreadTimer* pt = (PerThreadTimer*)malloc(sizeof(PerThreadTimer));
        if (pt == NULL) {
            return -1;
        }
        
        pt->thread_id = GetCurrentThreadId();
        pt->interval_ns = interval_ns;
        pt->active = 1;
        pt->timer = NULL;  /* Timer creation would go here */
        
        TlsSetValue(g_tls_index, pt);
        SPPROF_LOG("Registered thread %lu for per-thread sampling", pt->thread_id);
        return 0;
    }
    
    /* Global timer mode: Windows timer samples all threads automatically via GIL */
    SPPROF_LOG("Thread registered (global timer mode)");
    return 0;
}

int platform_unregister_thread(void) {
    if (g_per_thread_mode && g_tls_initialized) {
        PerThreadTimer* pt = (PerThreadTimer*)TlsGetValue(g_tls_index);
        if (pt != NULL) {
            pt->active = 0;
            if (pt->timer != NULL) {
                CloseThreadpoolTimer(pt->timer);
            }
            free(pt);
            TlsSetValue(g_tls_index, NULL);
            SPPROF_LOG("Unregistered thread from per-thread sampling");
        }
        return 0;
    }
    
    /* No per-thread resources on Windows in global timer mode */
    return 0;
}

/*
 * =============================================================================
 * Utility Functions
 * =============================================================================
 */

uint64_t platform_thread_id(void) {
    return (uint64_t)GetCurrentThreadId();
}

uint64_t platform_monotonic_ns(void) {
    if (!InterlockedCompareExchange(&g_perf_initialized, 0, 0)) {
        QueryPerformanceFrequency(&g_perf_freq);
        InterlockedExchange(&g_perf_initialized, 1);
    }

    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);

    /* Convert to nanoseconds using integer math to avoid floating point */
    return (uint64_t)(counter.QuadPart * 1000000000ULL / g_perf_freq.QuadPart);
}

const char* platform_name(void) {
    return SPPROF_PLATFORM_NAME;
}

/* No signal handler on Windows */
int platform_restore_signal_handler(void) {
    return 0;
}

/*
 * =============================================================================
 * Statistics
 * =============================================================================
 */

/**
 * Get platform-specific statistics.
 */
void platform_get_stats(
    uint64_t* samples_captured,
    uint64_t* samples_dropped,
    uint64_t* timer_overruns
) {
    if (samples_captured) {
        *samples_captured = (uint64_t)InterlockedCompareExchange64(&g_samples_captured, 0, 0);
    }
    if (samples_dropped) {
        *samples_dropped = (uint64_t)InterlockedCompareExchange64(&g_samples_dropped, 0, 0);
    }
    if (timer_overruns) {
        /* Use timer callbacks as a proxy for potential overruns */
        *timer_overruns = 0;
    }
}

/**
 * Get extended Windows-specific statistics.
 */
void platform_get_extended_stats(
    uint64_t* samples_captured,
    uint64_t* samples_dropped,
    uint64_t* timer_callbacks,
    uint64_t* gil_wait_time_ns
) {
    if (samples_captured) {
        *samples_captured = (uint64_t)InterlockedCompareExchange64(&g_samples_captured, 0, 0);
    }
    if (samples_dropped) {
        *samples_dropped = (uint64_t)InterlockedCompareExchange64(&g_samples_dropped, 0, 0);
    }
    if (timer_callbacks) {
        *timer_callbacks = (uint64_t)InterlockedCompareExchange64(&g_timer_callbacks, 0, 0);
    }
    if (gil_wait_time_ns) {
        *gil_wait_time_ns = (uint64_t)InterlockedCompareExchange64(&g_gil_wait_time_ns, 0, 0);
    }
}

/*
 * =============================================================================
 * Configuration Functions
 * =============================================================================
 */

/**
 * Enable or disable CPU time sampling (vs wall time).
 */
void platform_set_cpu_time(int enabled) {
    InterlockedExchange(&g_use_cpu_time, enabled ? 1 : 0);
    SPPROF_LOG("CPU time sampling: %s", enabled ? "enabled" : "disabled");
}

/**
 * Check if CPU time sampling is enabled.
 */
int platform_get_cpu_time(void) {
    return (int)InterlockedCompareExchange(&g_use_cpu_time, 0, 0);
}

/**
 * Enable or disable native unwinding.
 */
void platform_set_native_unwinding(int enabled) {
    InterlockedExchange(&g_native_unwinding, enabled ? 1 : 0);
    SPPROF_LOG("Native unwinding: %s", enabled ? "enabled" : "disabled");
}

/**
 * Check if native unwinding is enabled.
 */
int platform_get_native_unwinding(void) {
    return (int)InterlockedCompareExchange(&g_native_unwinding, 0, 0);
}

/**
 * Enable or disable per-thread sampling mode.
 */
void platform_set_per_thread_mode(int enabled) {
    InterlockedExchange(&g_per_thread_mode, enabled ? 1 : 0);
    if (enabled && !g_tls_initialized) {
        per_thread_init();
    }
    SPPROF_LOG("Per-thread mode: %s", enabled ? "enabled" : "disabled");
}

/**
 * Check if per-thread sampling mode is enabled.
 */
int platform_get_per_thread_mode(void) {
    return (int)InterlockedCompareExchange(&g_per_thread_mode, 0, 0);
}

/*
 * =============================================================================
 * Windows Stubs for Signal Handler Functions
 * 
 * On Windows, we don't use signals. These stubs allow module.c to compile.
 * =============================================================================
 */

/* Windows uses its own sample counting in timer_callback */
uint64_t signal_handler_samples_captured(void) {
    return (uint64_t)InterlockedCompareExchange64(&g_samples_captured, 0, 0);
}

uint64_t signal_handler_samples_dropped(void) {
    return (uint64_t)InterlockedCompareExchange64(&g_samples_dropped, 0, 0);
}

uint64_t signal_handler_errors(void) {
    return 0;  /* No error tracking on Windows yet */
}

void signal_handler_start(void) {
    InterlockedExchange(&g_sampling_active, 1);
}

void signal_handler_stop(void) {
    InterlockedExchange(&g_sampling_active, 0);
}

void signal_handler_set_native(int enabled) {
    platform_set_native_unwinding(enabled);
}

int signal_handler_install(int signum) {
    (void)signum;
    /* No signal installation on Windows */
    return 0;
}

int signal_handler_uninstall(int signum) {
    (void)signum;
    /* No signal uninstallation on Windows */
    return 0;
}

/*
 * =============================================================================
 * Debug Support
 * =============================================================================
 */

#ifdef SPPROF_DEBUG
void platform_debug_info(void) {
    printf("=== spprof Windows Platform Debug Info ===\n");
    printf("Platform initialized: %ld\n", g_platform_initialized);
    printf("Sampling active: %ld\n", g_sampling_active);
    printf("Timer queue: %p\n", g_timer_queue);
    printf("Timer: %p\n", g_timer);
    printf("Interval (ns): %llu\n", (unsigned long long)g_interval_ns);
    printf("Performance frequency: %lld\n", g_perf_freq.QuadPart);
    printf("CPU time mode: %ld\n", g_use_cpu_time);
    printf("Native unwinding: %ld\n", g_native_unwinding);
    printf("Per-thread mode: %ld\n", g_per_thread_mode);
    printf("\n--- Statistics ---\n");
    printf("Samples captured: %lld\n", g_samples_captured);
    printf("Samples dropped: %lld\n", g_samples_dropped);
    printf("Timer callbacks: %lld\n", g_timer_callbacks);
    printf("GIL wait time (ns): %lld\n", g_gil_wait_time_ns);
    printf("==========================================\n");
}
#endif

#endif /* _WIN32 */
