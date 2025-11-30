/**
 * unwind.h - Native C-stack unwinding via libunwind
 *
 * This module provides optional C-stack unwinding to capture native
 * frames (C/C++ functions) alongside Python frames. This enables
 * mixed-mode profiling where both Python and native code appear in
 * flame graphs.
 *
 * Platform support:
 *   - Linux: Full support via libunwind
 *   - macOS: Partial support via system unwind APIs
 *   - Windows: Not supported (would need DbgHelp/StackWalk64)
 *
 * IMPORTANT: Native unwinding adds overhead. Use only when debugging
 * C extensions or native code performance.
 */

#ifndef SPPROF_UNWIND_H
#define SPPROF_UNWIND_H

#include <stdint.h>
#include <stddef.h>

/* Maximum native frames to capture */
#define SPPROF_MAX_NATIVE_DEPTH 64

/* Native frame information */
typedef struct {
    uintptr_t ip;                    /* Instruction pointer */
    uintptr_t sp;                    /* Stack pointer */
    char symbol[256];                /* Symbol name (if resolved) */
    char filename[512];              /* Object file (if available) */
    uintptr_t offset;                /* Offset from symbol start */
    int resolved;                    /* 1 if symbol was resolved */
} NativeFrame;

/* Native stack sample */
typedef struct {
    NativeFrame frames[SPPROF_MAX_NATIVE_DEPTH];
    int depth;                       /* Number of valid frames */
    int truncated;                   /* 1 if stack was deeper than max */
} NativeStack;

/**
 * Check if native unwinding is available on this platform.
 *
 * @return 1 if available, 0 if not supported.
 */
int unwind_available(void);

/**
 * Initialize the unwinding subsystem.
 *
 * Must be called before any unwind_capture() calls.
 * Thread-safe: Can be called from multiple threads.
 *
 * @return 0 on success, -1 on error.
 */
int unwind_init(void);

/**
 * Shutdown the unwinding subsystem.
 */
void unwind_shutdown(void);

/**
 * Capture the current native call stack.
 *
 * This function walks the C stack using platform-specific unwinding.
 * It is NOT async-signal-safe on all platforms.
 *
 * @param stack Output structure to fill with frames.
 * @param skip_frames Number of frames to skip from top (to exclude
 *                    the profiler's own frames).
 * @return Number of frames captured, or -1 on error.
 */
int unwind_capture(NativeStack* stack, int skip_frames);

/**
 * Capture native stack with symbol resolution.
 *
 * This performs full symbol resolution using dladdr/backtrace_symbols.
 * More expensive than unwind_capture() but provides function names.
 *
 * NOT async-signal-safe. Use only from safe context.
 *
 * @param stack Output structure to fill.
 * @param skip_frames Frames to skip.
 * @return Number of frames captured, or -1 on error.
 */
int unwind_capture_with_symbols(NativeStack* stack, int skip_frames);

/**
 * Resolve symbols for a previously captured stack.
 *
 * Can be called later from a safe context if the original capture
 * was done in a signal handler.
 *
 * @param stack Stack to resolve symbols for.
 * @return Number of symbols resolved.
 */
int unwind_resolve_symbols(NativeStack* stack);

/**
 * Get platform-specific unwinding method name.
 *
 * @return String describing the unwinding method (e.g., "libunwind").
 */
const char* unwind_method_name(void);

#endif /* SPPROF_UNWIND_H */


