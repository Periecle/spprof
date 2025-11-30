/**
 * framewalker.h - Version-polymorphic Python frame walking
 *
 * This module provides a unified interface for walking Python call stacks
 * across different CPython versions (3.9-3.14). Each version has different
 * internal frame structures, so we use compile-time dispatch.
 *
 * CRITICAL: framewalker_capture() is called from the signal handler
 * and MUST be async-signal-safe.
 */

#ifndef SPPROF_FRAMEWALKER_H
#define SPPROF_FRAMEWALKER_H

#include <Python.h>
#include <stdint.h>

/**
 * RawFrameInfo - Minimal frame info captured in signal handler
 *
 * Contains only the raw code object pointer and shim flag.
 * String resolution happens later in the consumer thread.
 */
typedef struct {
    uintptr_t code_addr;  /* Raw PyCodeObject* pointer (may need untagging) */
    int is_shim;          /* 1 if FRAME_OWNED_BY_CSTACK (C extension frame) */
} RawFrameInfo;

/**
 * FrameWalkerVTable - Version-specific function pointers
 *
 * This table is populated at module init based on Python version.
 * Each function handles version-specific frame structure differences.
 */
typedef struct {
    /* Get current frame from thread state */
    void* (*get_current_frame)(PyThreadState* tstate);

    /* Get previous frame in chain */
    void* (*get_previous_frame)(void* frame);

    /* Extract code object address (handles tagging for 3.12+) */
    uintptr_t (*get_code_addr)(void* frame);

    /* Check if frame is a C-extension shim */
    int (*is_shim_frame)(void* frame);
} FrameWalkerVTable;

/**
 * Initialize the frame walker for the current Python version.
 *
 * Thread safety: NOT thread-safe. Call once at module init.
 * Async-signal safety: NO.
 *
 * @return 0 on success, -1 if Python version is unsupported.
 */
int framewalker_init(void);

/**
 * Walk the current thread's stack and capture frame pointers.
 *
 * Thread safety: Thread-safe (reads only from current thread).
 * Async-signal safety: YES - this is called from the signal handler.
 *
 * @param frames Output array to fill with frame info.
 * @param max_depth Maximum number of frames to capture.
 * @return Number of frames captured (0 to max_depth).
 */
int framewalker_capture(RawFrameInfo* frames, int max_depth);

/**
 * Capture frames as raw uintptr_t array (for ring buffer).
 *
 * Thread safety: Thread-safe.
 * Async-signal safety: YES.
 *
 * @param frame_ptrs Output array of code object pointers.
 * @param max_depth Maximum number of frames.
 * @return Number of frames captured.
 */
int framewalker_capture_raw(uintptr_t* frame_ptrs, int max_depth);

/**
 * Get Python version information string.
 *
 * @return String describing the detected Python version and frame walker.
 */
const char* framewalker_version_info(void);

/**
 * Get the active frame walker vtable (for testing).
 *
 * @return Pointer to the active FrameWalkerVTable.
 */
const FrameWalkerVTable* framewalker_get_vtable(void);

/**
 * Enable or disable native (C-stack) unwinding.
 *
 * When enabled, the frame walker will also capture native C/C++ frames
 * using libunwind (Linux) or backtrace (macOS). This enables mixed-mode
 * profiling but adds overhead.
 *
 * @param enabled 1 to enable, 0 to disable.
 * @return 0 on success, -1 if native unwinding not available.
 */
int framewalker_set_native_unwinding(int enabled);

/**
 * Check if native unwinding is enabled.
 *
 * @return 1 if enabled, 0 if disabled.
 */
int framewalker_native_unwinding_enabled(void);

/**
 * Check if native unwinding is available on this platform.
 *
 * @return 1 if available, 0 if not.
 */
int framewalker_native_unwinding_available(void);

#endif /* SPPROF_FRAMEWALKER_H */

