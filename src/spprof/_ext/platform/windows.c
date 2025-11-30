/**
 * platform/windows.c - Windows platform implementation
 *
 * Windows lacks POSIX signals. We use a timer queue timer with
 * thread suspension for sampling (similar to py-spy approach).
 */

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <Python.h>
#include <windows.h>
#include <processthreadsapi.h>

#include "platform.h"
#include "../ringbuffer.h"
#include "../framewalker.h"

/* Forward declaration */
extern RingBuffer* g_ringbuffer;

/* Global state */
static HANDLE g_timer_queue = NULL;
static HANDLE g_timer = NULL;
static HANDLE g_main_thread = NULL;
static uint64_t g_interval_ns = 0;
static LARGE_INTEGER g_perf_freq;
static int g_perf_initialized = 0;

/* Sample all threads */
static void CALLBACK timer_callback(PVOID param, BOOLEAN timer_fired) {
    extern RingBuffer* g_ringbuffer;

    if (g_ringbuffer == NULL) {
        return;
    }

    /* Get timestamp */
    uint64_t timestamp = platform_monotonic_ns();

    /* We need to acquire the GIL to enumerate Python threads safely */
    /* This is different from Unix where the signal is delivered in-thread */
    PyGILState_STATE gstate = PyGILState_Ensure();

    /* Get main interpreter state */
    PyInterpreterState* interp = PyInterpreterState_Main();
    if (interp == NULL) {
        PyGILState_Release(gstate);
        return;
    }

    /* Iterate over threads */
    PyThreadState* tstate = PyInterpreterState_ThreadHead(interp);
    while (tstate != NULL) {
        /* Get thread handle */
        HANDLE thread_handle = (HANDLE)tstate->thread_id;

        if (thread_handle != NULL && thread_handle != INVALID_HANDLE_VALUE) {
            /* Suspend the thread */
            DWORD suspend_count = SuspendThread(thread_handle);

            if (suspend_count != (DWORD)-1) {
                /* Capture sample */
                RawSample sample;
                sample.timestamp = timestamp;
                sample.thread_id = (uint64_t)GetThreadId(thread_handle);
                sample.depth = 0;
                sample._padding = 0;

                /* Note: On Windows, we need the frame from the suspended thread */
                /* For now, capture from thread state */
                void* frame = NULL;
#if PY_VERSION_HEX >= 0x030B0000
                if (tstate->cframe != NULL) {
                    frame = tstate->cframe->current_frame;
                }
#else
                frame = tstate->frame;
#endif

                /* Walk frames if available */
                if (frame != NULL) {
                    const FrameWalkerVTable* vtable = framewalker_get_vtable();
                    if (vtable != NULL) {
                        while (frame != NULL && sample.depth < SPPROF_MAX_STACK_DEPTH) {
                            if (!vtable->is_shim_frame(frame)) {
                                sample.frames[sample.depth] = vtable->get_code_addr(frame);
                                sample.depth++;
                            }
                            frame = vtable->get_previous_frame(frame);
                        }
                    }
                }

                /* Resume the thread */
                ResumeThread(thread_handle);

                /* Write sample if we got any frames */
                if (sample.depth > 0) {
                    ringbuffer_write(g_ringbuffer, &sample);
                }
            }
        }

        tstate = PyThreadState_Next(tstate);
    }

    PyGILState_Release(gstate);
}

int platform_init(void) {
    if (!g_perf_initialized) {
        QueryPerformanceFrequency(&g_perf_freq);
        g_perf_initialized = 1;
    }
    return 0;
}

void platform_cleanup(void) {
    platform_timer_destroy();
}

int platform_timer_create(uint64_t interval_ns) {
    g_interval_ns = interval_ns;

    /* Create timer queue */
    g_timer_queue = CreateTimerQueue();
    if (g_timer_queue == NULL) {
        return -1;
    }

    /* Calculate interval in milliseconds */
    DWORD interval_ms = (DWORD)(interval_ns / 1000000);
    if (interval_ms < 1) {
        interval_ms = 1;
    }

    /* Create timer */
    if (!CreateTimerQueueTimer(&g_timer, g_timer_queue,
                                timer_callback, NULL,
                                interval_ms, interval_ms,
                                WT_EXECUTEDEFAULT)) {
        DeleteTimerQueue(g_timer_queue);
        g_timer_queue = NULL;
        return -1;
    }

    return 0;
}

int platform_timer_destroy(void) {
    if (g_timer != NULL) {
        DeleteTimerQueueTimer(g_timer_queue, g_timer, INVALID_HANDLE_VALUE);
        g_timer = NULL;
    }

    if (g_timer_queue != NULL) {
        DeleteTimerQueue(g_timer_queue);
        g_timer_queue = NULL;
    }

    return 0;
}

uint64_t platform_thread_id(void) {
    return (uint64_t)GetCurrentThreadId();
}

uint64_t platform_monotonic_ns(void) {
    if (!g_perf_initialized) {
        platform_init();
    }

    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);

    /* Convert to nanoseconds */
    return (uint64_t)((double)counter.QuadPart * 1e9 / (double)g_perf_freq.QuadPart);
}

const char* platform_name(void) {
    return SPPROF_PLATFORM_NAME;
}

int platform_register_thread(uint64_t interval_ns) {
    /* Windows timer samples all threads automatically */
    return 0;
}

int platform_unregister_thread(void) {
    return 0;
}

/* No signal handler on Windows */
int platform_restore_signal_handler(void) {
    return 0;
}

#endif /* _WIN32 */


