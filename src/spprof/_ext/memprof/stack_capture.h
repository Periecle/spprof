/* SPDX-License-Identifier: MIT
 * stack_capture.h - Native and mixed-mode stack capture
 *
 * Captures native stack frames via frame pointer walking and integrates
 * with Python's frame walker for mixed-mode (Python + native) stacks.
 *
 * Frame Pointer Requirement:
 * The profiler relies on frame pointer walking which requires code to be
 * compiled with -fno-omit-frame-pointer. Many C extensions omit frame
 * pointers for performance, which will result in truncated stacks.
 */

#ifndef SPPROF_STACK_CAPTURE_H
#define SPPROF_STACK_CAPTURE_H

#include "memprof.h"
#include <stdint.h>

/* ============================================================================
 * Platform-Specific Address Validation
 * ============================================================================ */

#if defined(__x86_64__) || defined(_M_X64)
    #define ADDR_MAX_USER   0x00007FFFFFFFFFFFULL
    #define ADDR_ALIGN_MASK 0x7ULL  /* 8-byte alignment */
#elif defined(__aarch64__) || defined(_M_ARM64)
    #define ADDR_MAX_USER   0x0000FFFFFFFFFFFFULL
    #define ADDR_ALIGN_MASK 0x7ULL  /* 8-byte alignment */
#elif defined(__i386__)
    #define ADDR_MAX_USER   0xBFFFFFFFUL
    #define ADDR_ALIGN_MASK 0x3UL   /* 4-byte alignment */
#else
    /* Fallback: disable upper bound check */
    #define ADDR_MAX_USER   UINTPTR_MAX
    #define ADDR_ALIGN_MASK 0x7ULL
#endif

/* ============================================================================
 * Native Stack Capture
 * ============================================================================ */

/**
 * Capture native stack frames via frame pointer walking.
 *
 * CRITICAL: This function must NOT call malloc or any function that might.
 * It uses only stack-allocated data and direct memory reads.
 *
 * Requirements:
 *   - Compiled with -fno-omit-frame-pointer
 *   - Frame pointers present in target code
 *
 * @param frames     Output array for return addresses
 * @param max_depth  Maximum frames to capture
 * @param skip       Frames to skip (exclude profiler frames)
 * @return Number of frames captured
 */
int capture_native_stack(uintptr_t* frames, int max_depth, int skip);

/* ============================================================================
 * Mixed-Mode Stack Capture
 * ============================================================================ */

/**
 * Capture both Python and native frames.
 *
 * This function captures a unified stack trace containing both:
 *   1. Native frames (return addresses) - via frame pointer walking
 *   2. Python frames (function name, filename, line) - via framewalker.c
 *
 * @param out  Output structure with native and Python frames
 * @return Total frame count (native + Python)
 */
int capture_mixed_stack(MixedStackCapture* out);

/* ============================================================================
 * Symbol Resolution
 * ============================================================================ */

/**
 * Check if a frame is inside the Python interpreter core.
 *
 * Used during resolution to determine where to insert Python frames
 * in the merged stack trace.
 *
 * @param dli_fname  Shared object path from dladdr()
 * @param dli_sname  Symbol name from dladdr() (may be NULL)
 * @return 1 if Python interpreter frame, 0 otherwise
 */
int is_python_interpreter_frame(const char* dli_fname, const char* dli_sname);

/**
 * Resolve symbols for a stack entry.
 *
 * Populates function_names, file_names, and line_numbers arrays.
 * Uses dladdr for native symbols and Python code objects for Python frames.
 *
 * @param entry  Stack entry to resolve (modified in place)
 * @return 0 on success, -1 on error
 */
int resolve_stack_entry(StackEntry* entry);

/**
 * Resolve mixed-mode stack to array of resolved frames.
 *
 * Merges Python and native frames using "Trim & Sandwich" algorithm:
 *   - Native frames from leaf
 *   - Python frames inserted at interpreter boundary
 *   - Remaining native frames to root
 *
 * @param capture      Mixed stack capture from capture_mixed_stack()
 * @param out_frames   Output array of resolved frame strings
 * @param max_frames   Maximum frames to return
 * @return Number of frames resolved
 */
int resolve_mixed_stack(const MixedStackCapture* capture,
                        char** out_frames, int max_frames);

/* ============================================================================
 * Frame Pointer Health Tracking
 * ============================================================================ */

/**
 * Check frame pointer health and emit warning if needed.
 *
 * Heuristic: Deep Python + shallow native = likely missing frame pointers.
 *
 * @param native_depth  Number of native frames captured
 * @param python_depth  Number of Python frames captured
 */
void check_frame_pointer_health(int native_depth, int python_depth);

/**
 * Get frame pointer health statistics.
 *
 * @param out_shallow_warnings   Output: Number of truncated stacks
 * @param out_total_stacks       Output: Total stacks captured
 * @param out_avg_depth          Output: Average native depth
 * @param out_min_depth          Output: Minimum native depth observed
 */
void get_frame_pointer_health(uint64_t* out_shallow_warnings,
                               uint64_t* out_total_stacks,
                               float* out_avg_depth,
                               int* out_min_depth);

/* ============================================================================
 * Optional DWARF Unwinding (compile-time feature)
 * ============================================================================ */

#ifdef MEMPROF_USE_LIBUNWIND

/**
 * Capture native stack using DWARF unwinding (libunwind).
 *
 * WARNING: This is 100-1000x slower than frame pointer walking.
 * Use only for debugging or when frame pointers are unavailable.
 *
 * @param frames     Output array for return addresses
 * @param max_depth  Maximum frames to capture
 * @param skip       Frames to skip
 * @return Number of frames captured
 */
int capture_native_stack_dwarf(uintptr_t* frames, int max_depth, int skip);

#endif /* MEMPROF_USE_LIBUNWIND */

#endif /* SPPROF_STACK_CAPTURE_H */

