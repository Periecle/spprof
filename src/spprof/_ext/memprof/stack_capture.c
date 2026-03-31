/* SPDX-License-Identifier: MIT
 * stack_capture.c - Native and mixed-mode stack capture
 *
 * Captures native stack frames via frame pointer walking.
 *
 * ASYNC-SIGNAL-SAFETY:
 *   capture_native_stack() is async-signal-safe:
 *   - No malloc/free
 *   - No locks
 *   - Direct memory reads only
 *
 *   resolve_stack_entry() is NOT async-signal-safe:
 *   - Uses malloc for symbol strings
 *   - Uses dladdr/DbgHelp which may lock
 *
 * Copyright (c) 2024 spprof contributors
 */

/* _GNU_SOURCE for dladdr on Linux */
#if defined(__linux__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#endif

#include "stack_capture.h"
#include "memprof.h"
#include "../framewalker.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#include <intrin.h>
#pragma comment(lib, "dbghelp.lib")

/* DbgHelp initialization state */
static volatile LONG g_dbghelp_init = 0;
static SRWLOCK g_dbghelp_lock = SRWLOCK_INIT;
#endif

/* ============================================================================
 * String Interning Table
 *
 * Reduces memory usage by deduplicating symbol strings. Many stacks share
 * the same function names (e.g., "PyObject_Call", "numpy.core.multiarray.array")
 * so interning can reduce string memory usage by 90-95%.
 *
 * Implementation: Simple open-addressing hash table with FNV-1a hash.
 * Thread-safe via atomic operations on entry flags.
 *
 * OVERFLOW TRACKING (2024 Fix):
 *   When the hash table is full or has excessive collisions, string_intern()
 *   falls back to strdup(). These "overflow" strings are tracked in a separate
 *   linked list and freed during string_table_destroy() to prevent memory leaks.
 * ============================================================================ */

#define STRING_TABLE_SIZE 16384  /* Must be power of 2 */
#define STRING_TABLE_MASK (STRING_TABLE_SIZE - 1)

typedef struct {
    _Atomic uint32_t hash;   /* 0 = empty, non-zero = occupied */
    char* str;               /* Interned string (heap allocated) */
} StringTableEntry;

static StringTableEntry g_string_table[STRING_TABLE_SIZE] = {{0}};
static _Atomic uint32_t g_string_table_count = 0;

/* ============================================================================
 * Overflow String Tracking (for strdup fallback)
 *
 * When hash table is full, we fall back to strdup(). These strings must be
 * tracked separately for cleanup to prevent memory leaks.
 * ============================================================================ */

typedef struct OverflowString {
    char* str;
    struct OverflowString* next;
} OverflowString;

/* Lock-free overflow list using atomic pointer */
static _Atomic(OverflowString*) g_overflow_strings = NULL;
static _Atomic uint32_t g_overflow_count = 0;

/**
 * Track an overflow string for later cleanup.
 * Uses lock-free push to front of list.
 */
static void track_overflow_string(char* str) {
    if (!str) return;
    
    /* Allocate node (we're in the cold path, malloc is OK) */
    OverflowString* node = (OverflowString*)malloc(sizeof(OverflowString));
    if (!node) {
        /* If we can't track it, we have to leak it to avoid double-free */
        return;
    }
    
    node->str = str;
    
    /* Lock-free push to front of list */
    OverflowString* old_head;
    do {
        old_head = atomic_load_explicit(&g_overflow_strings, memory_order_relaxed);
        node->next = old_head;
    } while (!atomic_compare_exchange_weak_explicit(
        &g_overflow_strings, &old_head, node,
        memory_order_release, memory_order_relaxed));
    
    atomic_fetch_add_explicit(&g_overflow_count, 1, memory_order_relaxed);
}

/* FNV-1a hash for strings */
static uint32_t fnv1a_hash_str(const char* str) {
    if (!str) return 0;
    
    uint32_t hash = 2166136261u;  /* FNV offset basis */
    while (*str) {
        hash ^= (uint8_t)*str++;
        hash *= 16777619u;  /* FNV prime */
    }
    
    /* Ensure non-zero (0 = empty slot marker) */
    return hash ? hash : 1;
}

/**
 * Intern a string, returning a pointer to the canonical copy.
 * 
 * If the string is already in the table, returns the existing pointer.
 * If not, allocates a copy and stores it.
 * Thread-safe via CAS on hash field.
 *
 * OVERFLOW HANDLING (2024 Fix):
 *   When the hash table is full (after 64 probes), we fall back to strdup()
 *   and track the string in g_overflow_strings for proper cleanup.
 *
 * @param str  String to intern
 * @return Interned string pointer (never freed until shutdown), or NULL on error
 */
static char* string_intern(const char* str) {
    if (!str) return NULL;
    
    uint32_t hash = fnv1a_hash_str(str);
    uint32_t idx = hash & STRING_TABLE_MASK;
    
    for (int probe = 0; probe < 64; probe++) {
        StringTableEntry* entry = &g_string_table[idx];
        uint32_t entry_hash = atomic_load_explicit(&entry->hash, memory_order_acquire);
        
        /* Empty slot? Try to claim it */
        if (entry_hash == 0) {
            uint32_t expected = 0;
            if (atomic_compare_exchange_strong_explicit(
                    &entry->hash, &expected, hash,
                    memory_order_acq_rel, memory_order_relaxed)) {
                
                /* We claimed the slot - allocate and store string */
                entry->str = strdup(str);
                if (!entry->str) {
                    /* Allocation failed - release slot */
                    atomic_store_explicit(&entry->hash, 0, memory_order_release);
                    return NULL;
                }
                
                atomic_fetch_add_explicit(&g_string_table_count, 1, memory_order_relaxed);
                return entry->str;
            }
            
            /* CAS failed - re-read hash */
            entry_hash = atomic_load_explicit(&entry->hash, memory_order_acquire);
        }
        
        /* Check if this entry matches our string */
        if (entry_hash == hash && entry->str && strcmp(entry->str, str) == 0) {
            return entry->str;  /* Found existing copy */
        }
        
        /* Collision - linear probe */
        idx = (idx + 1) & STRING_TABLE_MASK;
    }
    
    /* Table full or excessive collisions - fall back to strdup.
     * MEMORY LEAK FIX: Track these overflow strings for cleanup. */
    char* overflow_str = strdup(str);
    if (overflow_str) {
        track_overflow_string(overflow_str);
    }
    return overflow_str;
}

/**
 * Cleanup string table (called at shutdown).
 * 
 * MEMORY LEAK FIX (2024): Also frees overflow strings that were strdup'd
 * when the hash table was full.
 */
void string_table_destroy(void) {
    /* Free main hash table strings */
    for (size_t i = 0; i < STRING_TABLE_SIZE; i++) {
        if (g_string_table[i].str) {
            free(g_string_table[i].str);
            g_string_table[i].str = NULL;
        }
        atomic_store_explicit(&g_string_table[i].hash, 0, memory_order_relaxed);
    }
    atomic_store_explicit(&g_string_table_count, 0, memory_order_relaxed);
    
    /* Free overflow strings (strdup fallback when hash table was full) */
    OverflowString* node = atomic_exchange_explicit(&g_overflow_strings, NULL,
                                                     memory_order_acquire);
    while (node) {
        OverflowString* next = node->next;
        if (node->str) {
            free(node->str);
        }
        free(node);
        node = next;
    }
    atomic_store_explicit(&g_overflow_count, 0, memory_order_relaxed);
}

#ifdef _WIN32

/**
 * Initialize DbgHelp for symbol resolution (thread-safe, lazy init).
 * 
 * @return 1 on success, 0 on failure
 */
static int init_dbghelp_for_memprof(void) {
    if (InterlockedCompareExchange(&g_dbghelp_init, 0, 0)) {
        return 1;  /* Already initialized */
    }
    
    AcquireSRWLockExclusive(&g_dbghelp_lock);
    
    if (g_dbghelp_init) {
        ReleaseSRWLockExclusive(&g_dbghelp_lock);
        return 1;
    }
    
    HANDLE process = GetCurrentProcess();
    
    SymSetOptions(
        SYMOPT_UNDNAME |
        SYMOPT_DEFERRED_LOADS |
        SYMOPT_LOAD_LINES
    );
    
    if (!SymInitialize(process, NULL, TRUE)) {
        ReleaseSRWLockExclusive(&g_dbghelp_lock);
        return 0;
    }
    
    InterlockedExchange(&g_dbghelp_init, 1);
    ReleaseSRWLockExclusive(&g_dbghelp_lock);
    return 1;
}

#else
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
    
    /*
     * Get current frame pointer (architecture-specific).
     *
     * MSVC x64 NOTE: MSVC doesn't support inline assembly on x64.
     * We use _AddressOfReturnAddress() intrinsic instead.
     * The stack layout is: [saved RBP][return address]
     * _AddressOfReturnAddress returns &return_address, so RBP is at -1.
     */
#if defined(_MSC_VER)
    /* MSVC: Use intrinsic for all architectures */
    fp = (void*)((uintptr_t*)_AddressOfReturnAddress() - 1);
#elif defined(__x86_64__)
    __asm__ volatile("mov %%rbp, %0" : "=r"(fp));
#elif defined(__aarch64__)
    __asm__ volatile("mov %0, x29" : "=r"(fp));
#elif defined(__i386__)
    __asm__ volatile("mov %%ebp, %0" : "=r"(fp));
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
                        /* STRING INTERNING: Python function/file names are highly repetitive */
                        entry->function_names[out_idx] = string_intern(func_name);
                        entry->file_names[out_idx] = string_intern(file_name);
                        entry->line_numbers[out_idx] = line_no;
                        /* Free the original strings from resolve_code_object */
                        free(func_name);
                        free(file_name);
                    } else {
                        char buf[32];
                        snprintf(buf, sizeof(buf), "<python:0x%lx>", 
                                 (unsigned long)entry->python_frames[p]);
                        entry->function_names[out_idx] = string_intern(buf);
                        entry->file_names[out_idx] = string_intern("<python>");
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
                    /* STRING INTERNING: Reuse existing string if already seen */
                    entry->function_names[out_idx] = string_intern(info.dli_sname);
                } else {
                    char buf[32];
                    snprintf(buf, sizeof(buf), "0x%lx", (unsigned long)entry->frames[i]);
                    entry->function_names[out_idx] = string_intern(buf);
                }
                
                if (info.dli_fname) {
                    /* STRING INTERNING: File paths are highly repetitive */
                    entry->file_names[out_idx] = string_intern(info.dli_fname);
                } else {
                    entry->file_names[out_idx] = string_intern("<unknown>");
                }
                
                entry->line_numbers[out_idx] = 0;
                out_idx++;
            }
        } else {
            char buf[32];
            snprintf(buf, sizeof(buf), "0x%lx", (unsigned long)entry->frames[i]);
            entry->function_names[out_idx] = string_intern(buf);
            entry->file_names[out_idx] = string_intern("<unknown>");
            entry->line_numbers[out_idx] = 0;
            out_idx++;
        }
    }
#else
    /* Windows: Use DbgHelp for symbol resolution */
    HANDLE process = GetCurrentProcess();
    int have_dbghelp = init_dbghelp_for_memprof();
    
    /* Allocate symbol info buffer */
    char symbol_buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(TCHAR)];
    PSYMBOL_INFO symbol = (PSYMBOL_INFO)symbol_buffer;
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;
    
    for (int i = 0; i < entry->depth && out_idx < total_depth; i++) {
        char func_buf[256];
        char file_buf[MAX_PATH];
        int line_no = 0;
        
        if (have_dbghelp) {
            DWORD64 displacement = 0;
            if (SymFromAddr(process, (DWORD64)entry->frames[i], &displacement, symbol)) {
                if (displacement > 0) {
                    snprintf(func_buf, sizeof(func_buf), "%s+0x%llx",
                             symbol->Name, (unsigned long long)displacement);
                } else {
                    strncpy(func_buf, symbol->Name, sizeof(func_buf) - 1);
                    func_buf[sizeof(func_buf) - 1] = '\0';
                }
            } else {
                snprintf(func_buf, sizeof(func_buf), "0x%llx",
                         (unsigned long long)entry->frames[i]);
            }
            
            /* Try to get source file and line */
            IMAGEHLP_LINE64 line;
            line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
            DWORD line_displacement = 0;
            
            if (SymGetLineFromAddr64(process, (DWORD64)entry->frames[i],
                                      &line_displacement, &line)) {
                strncpy(file_buf, line.FileName, sizeof(file_buf) - 1);
                file_buf[sizeof(file_buf) - 1] = '\0';
                line_no = (int)line.LineNumber;
            } else {
                strcpy(file_buf, "<unknown>");
            }
        } else {
            /* DbgHelp not available - fallback to hex address */
            snprintf(func_buf, sizeof(func_buf), "0x%llx",
                     (unsigned long long)entry->frames[i]);
            strcpy(file_buf, "<unknown>");
        }
        
        /* STRING INTERNING: Windows symbol names */
        entry->function_names[out_idx] = string_intern(func_buf);
        entry->file_names[out_idx] = string_intern(file_buf);
        entry->line_numbers[out_idx] = line_no;
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
                out_frames[out_idx++] = string_intern(buf);
            }
        }
    }
#else
    /* Windows: Use DbgHelp for symbol resolution */
    HANDLE process = GetCurrentProcess();
    int have_dbghelp = init_dbghelp_for_memprof();
    
    char symbol_buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(TCHAR)];
    PSYMBOL_INFO symbol = (PSYMBOL_INFO)symbol_buffer;
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;
    
    for (int i = 0; i < capture->native_depth && out_idx < max_frames; i++) {
        char buf[256];
        
        if (have_dbghelp) {
            DWORD64 displacement = 0;
            if (SymFromAddr(process, (DWORD64)capture->native_pcs[i], &displacement, symbol)) {
                snprintf(buf, sizeof(buf), "%s", symbol->Name);
            } else {
                snprintf(buf, sizeof(buf), "0x%llx",
                         (unsigned long long)capture->native_pcs[i]);
            }
        } else {
            snprintf(buf, sizeof(buf), "0x%llx",
                     (unsigned long long)capture->native_pcs[i]);
        }
        
        out_frames[out_idx++] = string_intern(buf);
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
    
    /* Suspicious: Deep Python call stack but native stack truncated.
     * 
     * Instead of printing to stderr (bad for library code), we track this
     * via atomic counter. Applications can check frame pointer health via
     * get_frame_pointer_health() and emit their own warnings if needed.
     */
    if (native_depth < 3 && python_depth > 5) {
        atomic_fetch_add_explicit(&g_memprof.shallow_stack_warnings, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_fp_warning_emitted, 1, memory_order_relaxed);
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

