/**
 * unwind.c - Native C-stack unwinding implementation
 *
 * Platform-specific implementations for capturing native call stacks.
 *
 * Linux: Uses libunwind (if available) or glibc backtrace()
 * macOS: Uses execinfo backtrace() or _Unwind_Backtrace
 * Windows: Not implemented (would need DbgHelp)
 */

/* Must be before any includes for dladdr */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "unwind.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* Platform detection */
#if defined(__linux__)
    #define SPPROF_UNWIND_LINUX 1
#elif defined(__APPLE__)
    #define SPPROF_UNWIND_DARWIN 1
#elif defined(_WIN32)
    #define SPPROF_UNWIND_WINDOWS 1
#endif

/* Try to use libunwind on Linux if available */
/* Only use libunwind if explicitly enabled via SPPROF_HAS_LIBUNWIND define */
#if defined(SPPROF_HAS_LIBUNWIND) && defined(SPPROF_UNWIND_LINUX)
    #define UNW_LOCAL_ONLY
    #include <libunwind.h>
#endif

/* Fall back to glibc backtrace on Linux/macOS */
#if defined(SPPROF_UNWIND_LINUX) || defined(SPPROF_UNWIND_DARWIN)
    #include <execinfo.h>
    #include <dlfcn.h>
    #define SPPROF_HAS_BACKTRACE 1
#endif

static int g_initialized = 0;

int unwind_available(void) {
#if defined(SPPROF_HAS_LIBUNWIND) || defined(SPPROF_HAS_BACKTRACE)
    return 1;
#else
    return 0;
#endif
}

int unwind_init(void) {
    if (g_initialized) {
        return 0;
    }

#if defined(SPPROF_HAS_LIBUNWIND)
    /* libunwind doesn't require explicit init */
#endif

    g_initialized = 1;
    return 0;
}

void unwind_shutdown(void) {
    g_initialized = 0;
}

const char* unwind_method_name(void) {
#if defined(SPPROF_HAS_LIBUNWIND)
    return "libunwind";
#elif defined(SPPROF_HAS_BACKTRACE)
    return "backtrace";
#elif defined(SPPROF_UNWIND_WINDOWS)
    return "none (Windows not supported)";
#else
    return "none";
#endif
}

#if defined(SPPROF_HAS_LIBUNWIND)
/*
 * libunwind-based stack capture (Linux)
 *
 * libunwind provides accurate unwinding even through signal handlers
 * and supports both local and remote unwinding.
 */

int unwind_capture(NativeStack* stack, int skip_frames) {
    if (!g_initialized || stack == NULL) {
        return -1;
    }

    memset(stack, 0, sizeof(NativeStack));

    unw_cursor_t cursor;
    unw_context_t context;

    /* Initialize cursor to current frame */
    if (unw_getcontext(&context) < 0) {
        return -1;
    }

    if (unw_init_local(&cursor, &context) < 0) {
        return -1;
    }

    int frame_idx = 0;
    int skipped = 0;

    /* Walk the stack */
    while (unw_step(&cursor) > 0 && frame_idx < SPPROF_MAX_NATIVE_DEPTH) {
        /* Skip requested frames */
        if (skipped < skip_frames) {
            skipped++;
            continue;
        }

        NativeFrame* frame = &stack->frames[frame_idx];

        /* Get instruction pointer */
        unw_word_t ip;
        if (unw_get_reg(&cursor, UNW_REG_IP, &ip) < 0) {
            break;
        }
        frame->ip = (uintptr_t)ip;

        /* Get stack pointer */
        unw_word_t sp;
        if (unw_get_reg(&cursor, UNW_REG_SP, &sp) == 0) {
            frame->sp = (uintptr_t)sp;
        }

        /* Try to get procedure name */
        unw_word_t offset;
        if (unw_get_proc_name(&cursor, frame->symbol, sizeof(frame->symbol), &offset) == 0) {
            frame->offset = (uintptr_t)offset;
            frame->resolved = 1;
        } else {
            snprintf(frame->symbol, sizeof(frame->symbol), "0x%lx", (unsigned long)ip);
            frame->resolved = 0;
        }

        frame_idx++;
    }

    stack->depth = frame_idx;
    stack->truncated = (unw_step(&cursor) > 0) ? 1 : 0;

    return frame_idx;
}

int unwind_capture_with_symbols(NativeStack* stack, int skip_frames) {
    /* libunwind already resolves symbols in unwind_capture */
    return unwind_capture(stack, skip_frames);
}

int unwind_resolve_symbols(NativeStack* stack) {
    if (stack == NULL) {
        return -1;
    }

    int resolved = 0;

    for (int i = 0; i < stack->depth; i++) {
        NativeFrame* frame = &stack->frames[i];

        if (frame->resolved) {
            resolved++;
            continue;
        }

        /* Use dladdr for additional resolution */
        Dl_info info;
        if (dladdr((void*)frame->ip, &info) != 0) {
            if (info.dli_sname != NULL) {
                strncpy(frame->symbol, info.dli_sname, sizeof(frame->symbol) - 1);
                frame->symbol[sizeof(frame->symbol) - 1] = '\0';
                frame->resolved = 1;
                resolved++;
            }
            if (info.dli_fname != NULL) {
                strncpy(frame->filename, info.dli_fname, sizeof(frame->filename) - 1);
                frame->filename[sizeof(frame->filename) - 1] = '\0';
            }
            if (info.dli_saddr != NULL) {
                frame->offset = frame->ip - (uintptr_t)info.dli_saddr;
            }
        }
    }

    return resolved;
}

#elif defined(SPPROF_HAS_BACKTRACE)
/*
 * glibc/execinfo backtrace-based capture (Linux/macOS fallback)
 *
 * Less accurate than libunwind but widely available.
 */

int unwind_capture(NativeStack* stack, int skip_frames) {
    if (!g_initialized || stack == NULL) {
        return -1;
    }

    memset(stack, 0, sizeof(NativeStack));

    /* Capture raw addresses */
    void* buffer[SPPROF_MAX_NATIVE_DEPTH + 16];  /* Extra for skipping */
    int total_frames = backtrace(buffer, SPPROF_MAX_NATIVE_DEPTH + skip_frames + 1);

    if (total_frames <= skip_frames) {
        return 0;
    }

    int frame_idx = 0;
    for (int i = skip_frames + 1; i < total_frames && frame_idx < SPPROF_MAX_NATIVE_DEPTH; i++) {
        NativeFrame* frame = &stack->frames[frame_idx];
        frame->ip = (uintptr_t)buffer[i];
        frame->sp = 0;  /* backtrace doesn't provide SP */
        frame->resolved = 0;

        /* Format address as hex */
        snprintf(frame->symbol, sizeof(frame->symbol), "0x%lx", (unsigned long)frame->ip);

        frame_idx++;
    }

    stack->depth = frame_idx;
    stack->truncated = (total_frames > SPPROF_MAX_NATIVE_DEPTH + skip_frames + 1) ? 1 : 0;

    return frame_idx;
}

int unwind_capture_with_symbols(NativeStack* stack, int skip_frames) {
    int captured = unwind_capture(stack, skip_frames);
    if (captured > 0) {
        unwind_resolve_symbols(stack);
    }
    return captured;
}

int unwind_resolve_symbols(NativeStack* stack) {
    if (stack == NULL) {
        return -1;
    }

    int resolved = 0;

    for (int i = 0; i < stack->depth; i++) {
        NativeFrame* frame = &stack->frames[i];

        /* Use dladdr for symbol resolution */
        Dl_info info;
        if (dladdr((void*)frame->ip, &info) != 0) {
            if (info.dli_sname != NULL) {
                strncpy(frame->symbol, info.dli_sname, sizeof(frame->symbol) - 1);
                frame->symbol[sizeof(frame->symbol) - 1] = '\0';
                frame->resolved = 1;
                resolved++;
            }
            if (info.dli_fname != NULL) {
                strncpy(frame->filename, info.dli_fname, sizeof(frame->filename) - 1);
                frame->filename[sizeof(frame->filename) - 1] = '\0';
            }
            if (info.dli_saddr != NULL) {
                frame->offset = frame->ip - (uintptr_t)info.dli_saddr;
            }
        }
    }

    return resolved;
}

#else
/*
 * No unwinding support (Windows or unknown platform)
 */

int unwind_capture(NativeStack* stack, int skip_frames) {
    (void)skip_frames;
    if (stack != NULL) {
        memset(stack, 0, sizeof(NativeStack));
    }
    return 0;
}

int unwind_capture_with_symbols(NativeStack* stack, int skip_frames) {
    return unwind_capture(stack, skip_frames);
}

int unwind_resolve_symbols(NativeStack* stack) {
    (void)stack;
    return 0;
}

#endif /* Platform selection */


