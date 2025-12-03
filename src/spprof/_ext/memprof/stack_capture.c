/* SPDX-License-Identifier: MIT
 * stack_capture.c - Native and mixed-mode stack capture
 *
 * Captures native stack frames via frame pointer walking.
 */

#include "stack_capture.h"
#include "memprof.h"
#include "../framewalker.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef _WIN32
#include <dlfcn.h>
#endif

/* ============================================================================
 * Frame Pointer Health Tracking
 * ============================================================================ */

static _Atomic uint64_t g_total_native_stacks = 0;
static _Atomic uint64_t g_total_native_depth = 0;
static _Atomic int g_min_native_depth = 1000;
static _Atomic int g_fp_warning_emitted = 0;

/* ============================================================================
 * Native Stack Capture (Frame Pointer Walking)
 * ============================================================================ */

int capture_native_stack(uintptr_t* frames, int max_depth, int skip) {
    if (!frames || max_depth <= 0) {
        return 0;
    }
    
    int depth = 0;
    void* fp = NULL;
    
    /* Get current frame pointer (architecture-specific) */
#if defined(__x86_64__) || defined(_M_X64)
    __asm__ volatile("mov %%rbp, %0" : "=r"(fp));
#elif defined(__aarch64__) || defined(_M_ARM64)
    __asm__ volatile("mov %0, x29" : "=r"(fp));
#elif defined(__i386__)
    __asm__ volatile("mov %%ebp, %0" : "=r"(fp));
#elif defined(_MSC_VER)
    /* MSVC: Use _AddressOfReturnAddress */
    fp = (void*)((uintptr_t*)_AddressOfReturnAddress() - 1);
#else
    fp = __builtin_frame_address(0);
#endif

    /* Clamp skip to reasonable value */
    if (skip < 0) skip = 0;

    while (fp && depth < max_depth + skip) {
        uintptr_t fp_val = (uintptr_t)fp;
        
        /* Validate frame pointer using platform-specific bounds */
        if (fp_val < 0x1000) break;                    /* NULL-ish (first page unmapped) */
        if (fp_val > ADDR_MAX_USER) break;             /* Kernel space */
        if ((fp_val & ADDR_ALIGN_MASK) != 0) break;    /* Misaligned */
        
        /* Read frame: [prev_fp, return_addr] */
        void** frame = (void**)fp;
        void* ret_addr = frame[1];
        void* prev_fp = frame[0];
        
        /* Validate return address */
        if (!ret_addr) break;
        if ((uintptr_t)ret_addr < 0x1000) break;
        
        /* Detect infinite loop (corrupted stack) */
        if ((uintptr_t)prev_fp <= fp_val && prev_fp != NULL) break;
        
        /* Store frame if past skip count */
        if (depth >= skip && (depth - skip) < max_depth) {
            frames[depth - skip] = (uintptr_t)ret_addr;
        }
        
        depth++;
        fp = prev_fp;
    }
    
    return (depth > skip) ? (depth - skip) : 0;
}

/* ============================================================================
 * Mixed-Mode Stack Capture
 * ============================================================================ */

/* Forward declaration - implemented in framewalker.c */
extern int framewalker_capture_raw(uintptr_t* code_ptrs, int max_depth);

int capture_mixed_stack(MixedStackCapture* out) {
    if (!out) return 0;
    
    memset(out, 0, sizeof(*out));
    
    /* 1. Capture native frames (fast, no allocations) */
    out->native_depth = capture_native_stack(out->native_pcs, MEMPROF_MAX_STACK_DEPTH, 3);
    
    /* 2. Capture Python frames using existing framewalker infrastructure
     * Note: This may not be available in all contexts (e.g., if called from
     * outside Python interpreter). In that case, we just use native frames. */
#ifdef SPPROF_HAS_FRAMEWALKER
    out->python_depth = framewalker_capture_raw(out->python_code_ptrs, MEMPROF_MAX_STACK_DEPTH);
#else
    out->python_depth = 0;
#endif
    
    return out->native_depth + out->python_depth;
}

/* ============================================================================
 * Python Interpreter Frame Detection
 * ============================================================================ */

int is_python_interpreter_frame(const char* dli_fname, const char* dli_sname) {
    if (!dli_fname) {
        return 0;
    }
    
    /* Check shared object name for "python" */
    /* Match: libpython3.11.so, python311.dll, Python.framework, etc. */
    if (strstr(dli_fname, "python") || strstr(dli_fname, "Python")) {
        /* Verify it's the interpreter, not a C extension with "python" in name */
        if (dli_sname) {
            /* Core interpreter functions we want to skip */
            if (strncmp(dli_sname, "PyEval_", 7) == 0 ||
                strncmp(dli_sname, "_PyEval_", 8) == 0 ||
                strncmp(dli_sname, "PyObject_", 9) == 0 ||
                strncmp(dli_sname, "_PyObject_", 10) == 0 ||
                strncmp(dli_sname, "PyFrame_", 8) == 0 ||
                strcmp(dli_sname, "pymain_run_python") == 0 ||
                strcmp(dli_sname, "Py_RunMain") == 0) {
                return 1;
            }
        }
        /* No symbol name but in Python library - likely interpreter */
        if (!dli_sname) {
            return 1;
        }
    }
    
    return 0;
}

/* ============================================================================
 * Symbol Resolution
 * ============================================================================ */

int resolve_stack_entry(StackEntry* entry) {
    if (!entry || entry->depth == 0) {
        return -1;
    }
    
    /* Check if already resolved */
    if (entry->flags & STACK_FLAG_RESOLVED) {
        return 0;
    }
    
    /* Calculate total frames: native + python */
    int total_depth = entry->depth + entry->python_depth;
    if (total_depth <= 0 || total_depth > MEMPROF_MAX_STACK_DEPTH * 2) {
        return -1;  /* Invalid depth */
    }
    
    /* Allocate arrays for resolved symbols */
    entry->function_names = (char**)calloc((size_t)total_depth, sizeof(char*));
    entry->file_names = (char**)calloc((size_t)total_depth, sizeof(char*));
    entry->line_numbers = (int*)calloc((size_t)total_depth, sizeof(int));
    
    if (!entry->function_names || !entry->file_names || !entry->line_numbers) {
        free(entry->function_names);
        free(entry->file_names);
        free(entry->line_numbers);
        entry->function_names = NULL;
        entry->file_names = NULL;
        entry->line_numbers = NULL;
        return -1;
    }
    
    int out_idx = 0;
    int python_inserted = 0;
    
#ifndef _WIN32
    /* POSIX: Use dladdr for native frames */
    for (int i = 0; i < entry->depth && out_idx < total_depth; i++) {
        Dl_info info;
        int is_interpreter = 0;
        
        if (dladdr((void*)entry->frames[i], &info)) {
            is_interpreter = is_python_interpreter_frame(info.dli_fname, info.dli_sname);
            
            /* Insert Python frames at interpreter boundary */
            if (is_interpreter && !python_inserted && entry->python_depth > 0) {
#ifdef SPPROF_HAS_FRAMEWALKER
                /* Insert all Python frames here */
                for (int p = 0; p < entry->python_depth && out_idx < total_depth; p++) {
                    char* func_name = NULL;
                    char* file_name = NULL;
                    int line_no = 0;
                    
                    if (resolve_code_object(entry->python_frames[p], 
                                           &func_name, &file_name, &line_no) == 0) {
                        entry->function_names[out_idx] = func_name;
                        entry->file_names[out_idx] = file_name;
                        entry->line_numbers[out_idx] = line_no;
                    } else {
                        char buf[32];
                        snprintf(buf, sizeof(buf), "<python:0x%lx>", 
                                 (unsigned long)entry->python_frames[p]);
                        entry->function_names[out_idx] = strdup(buf);
                        entry->file_names[out_idx] = strdup("<python>");
                        entry->line_numbers[out_idx] = 0;
                    }
                    out_idx++;
                }
#endif
                python_inserted = 1;
            }
            
            /* Add native frame (skip interpreter frames after Python insertion) */
            if (!is_interpreter || !python_inserted) {
                if (info.dli_sname) {
                    entry->function_names[out_idx] = strdup(info.dli_sname);
                } else {
                    char buf[32];
                    snprintf(buf, sizeof(buf), "0x%lx", (unsigned long)entry->frames[i]);
                    entry->function_names[out_idx] = strdup(buf);
                }
                
                if (info.dli_fname) {
                    entry->file_names[out_idx] = strdup(info.dli_fname);
                } else {
                    entry->file_names[out_idx] = strdup("<unknown>");
                }
                
                entry->line_numbers[out_idx] = 0;
                out_idx++;
            }
        } else {
            char buf[32];
            snprintf(buf, sizeof(buf), "0x%lx", (unsigned long)entry->frames[i]);
            entry->function_names[out_idx] = strdup(buf);
            entry->file_names[out_idx] = strdup("<unknown>");
            entry->line_numbers[out_idx] = 0;
            out_idx++;
        }
    }
#else
    /* Windows: Would use DbgHelp - for now just use addresses */
    for (int i = 0; i < entry->depth && out_idx < total_depth; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)entry->frames[i]);
        entry->function_names[out_idx] = strdup(buf);
        entry->file_names[out_idx] = strdup("<unknown>");
        entry->line_numbers[out_idx] = 0;
        out_idx++;
    }
#endif
    
    /* Update depth to reflect merged stack */
    entry->depth = (uint16_t)out_idx;
    entry->flags |= STACK_FLAG_RESOLVED;
    return 0;
}

/* ============================================================================
 * Mixed-Mode Resolution
 * ============================================================================ */

int resolve_mixed_stack(const MixedStackCapture* capture,
                        char** out_frames, int max_frames) {
    if (!capture || !out_frames || max_frames <= 0) {
        return 0;
    }
    
    int out_idx = 0;
    int python_inserted = 0;
    
#ifndef _WIN32
    for (int i = 0; i < capture->native_depth && out_idx < max_frames; i++) {
        Dl_info info;
        if (dladdr((void*)capture->native_pcs[i], &info)) {
            int is_interpreter = is_python_interpreter_frame(info.dli_fname, info.dli_sname);
            
            if (is_interpreter && !python_inserted) {
                /* Insert Python frames here */
                /* TODO: Integrate with Python frame resolution */
                python_inserted = 1;
                /* Skip interpreter frames */
            } else if (!is_interpreter) {
                /* Include non-interpreter native frame */
                char buf[256];
                const char* name = info.dli_sname ? info.dli_sname : "<unknown>";
                snprintf(buf, sizeof(buf), "%s", name);
                out_frames[out_idx++] = strdup(buf);
            }
        }
    }
#else
    /* Windows: Just use native frames for now */
    for (int i = 0; i < capture->native_depth && out_idx < max_frames; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)capture->native_pcs[i]);
        out_frames[out_idx++] = strdup(buf);
    }
#endif
    
    return out_idx;
}

/* ============================================================================
 * Frame Pointer Health
 * ============================================================================ */

void check_frame_pointer_health(int native_depth, int python_depth) {
    /* Update statistics */
    atomic_fetch_add_explicit(&g_total_native_stacks, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_total_native_depth, (uint64_t)native_depth, memory_order_relaxed);
    
    /* Update min depth (relaxed - doesn't need to be precise) */
    int prev_min = atomic_load_explicit(&g_min_native_depth, memory_order_relaxed);
    if (native_depth < prev_min) {
        atomic_store_explicit(&g_min_native_depth, native_depth, memory_order_relaxed);
    }
    
    /* Suspicious: Deep Python call stack but native stack truncated */
    if (native_depth < 3 && python_depth > 5) {
        atomic_fetch_add_explicit(&g_memprof.shallow_stack_warnings, 1, memory_order_relaxed);
        
        /* Emit one-time warning (first 10 occurrences) */
        int prev = atomic_fetch_add_explicit(&g_fp_warning_emitted, 1, memory_order_relaxed);
        if (prev < 10) {
            fprintf(stderr,
                "[spprof] WARNING: Native stacks truncated (depth=%d). "
                "C extensions may be compiled without frame pointers.\n"
                "For full stack traces, rebuild extensions with:\n"
                "  CFLAGS='-fno-omit-frame-pointer' pip install --no-binary :all: <package>\n"
                "Or use debug builds of NumPy/SciPy.\n",
                native_depth);
        }
        if (prev == 9) {
            fprintf(stderr, "[spprof] (Suppressing further frame pointer warnings)\n");
        }
    }
}

void get_frame_pointer_health(uint64_t* out_shallow_warnings,
                               uint64_t* out_total_stacks,
                               float* out_avg_depth,
                               int* out_min_depth) {
    if (out_shallow_warnings) {
        *out_shallow_warnings = atomic_load_explicit(&g_memprof.shallow_stack_warnings,
                                                      memory_order_relaxed);
    }
    
    if (out_total_stacks) {
        *out_total_stacks = atomic_load_explicit(&g_total_native_stacks, memory_order_relaxed);
    }
    
    if (out_avg_depth) {
        uint64_t total = atomic_load_explicit(&g_total_native_stacks, memory_order_relaxed);
        uint64_t depth_sum = atomic_load_explicit(&g_total_native_depth, memory_order_relaxed);
        *out_avg_depth = (total > 0) ? (float)depth_sum / (float)total : 0.0f;
    }
    
    if (out_min_depth) {
        int min = atomic_load_explicit(&g_min_native_depth, memory_order_relaxed);
        *out_min_depth = (min == 1000) ? 0 : min;
    }
}

/* ============================================================================
 * Optional DWARF Unwinding
 * ============================================================================ */

#ifdef MEMPROF_USE_LIBUNWIND
#include <libunwind.h>

int capture_native_stack_dwarf(uintptr_t* frames, int max_depth, int skip) {
    unw_cursor_t cursor;
    unw_context_t context;
    
    unw_getcontext(&context);
    unw_init_local(&cursor, &context);
    
    int depth = 0;
    while (depth < max_depth + skip && unw_step(&cursor) > 0) {
        unw_word_t pc;
        unw_get_reg(&cursor, UNW_REG_IP, &pc);
        if (depth >= skip) {
            frames[depth - skip] = (uintptr_t)pc;
        }
        depth++;
    }
    
    return (depth > skip) ? (depth - skip) : 0;
}

#endif /* MEMPROF_USE_LIBUNWIND */

