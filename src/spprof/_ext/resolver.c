/**
 * resolver.c - Symbol resolution from raw frame pointers
 *
 * The resolver runs in a background thread, consuming samples from
 * the ring buffer and resolving PyCodeObject* pointers to readable
 * function names, filenames, and line numbers.
 *
 * MIXED-MODE PROFILING:
 * This resolver also handles native C frames captured via frame pointer
 * walking. Native symbols are resolved via dladdr() which is safe to call
 * here (outside of thread suspension context).
 *
 * The "Trim & Sandwich" algorithm merges native and Python frames:
 *   1. Walk native stack from leaf (most recent)
 *   2. Include native frames until we hit the Python interpreter
 *   3. Insert the Python stack at that point
 *   4. Optionally continue with remaining native frames (main/entry)
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

/* Platform-specific includes for native symbol resolution */
#if defined(__APPLE__) || defined(__linux__)
#define SPPROF_HAS_DLADDR 1
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <pthread.h>
#endif

#ifdef _WIN32
#include <windows.h>
#endif

#include "resolver.h"
#include "code_registry.h"

/*
 * _Py_CODEUNIT is an internal type not exposed in public headers for Python 3.13+.
 * We define our own compatible version for line number calculation.
 * Each instruction is a fixed-width 2-byte value: 1-byte opcode + 1-byte oparg.
 */
#if PY_VERSION_HEX >= 0x030D0000
typedef union {
    uint16_t cache;
    struct {
        uint8_t code;
        uint8_t arg;
    } op;
} _Py_CODEUNIT;
#endif

/*
 * =============================================================================
 * Symbol Resolution Cache (4-Way Set-Associative with Pseudo-LRU)
 * =============================================================================
 *
 * Design rationale:
 * - Direct-mapped cache has high collision rate due to pointer alignment
 *   (PyCodeObject* are typically 8-16 byte aligned, wasting lower hash bits)
 * - 4-way set-associative reduces collision eviction by 4x
 * - Pseudo-LRU (tree-based) provides near-optimal eviction with minimal overhead
 * - Better hash function uses middle bits and multiplicative mixing
 *
 * Memory layout:
 * - 1024 sets × 4 ways = 4096 total entries (same as before)
 * - Each set has 3-bit LRU state for tree-based pseudo-LRU
 */
#define CACHE_WAYS 4
#define CACHE_SETS 1024
#define CACHE_SET_MASK (CACHE_SETS - 1)

/* Forward declarations */
static int resolve_code_object_with_instr(uintptr_t code_addr, uintptr_t instr_ptr, ResolvedFrame* out);

typedef struct {
    uintptr_t key;
    ResolvedFrame value;
    int valid;
} CacheEntry;

typedef struct {
    CacheEntry ways[CACHE_WAYS];
    uint8_t lru_bits;  /* 3 bits for tree-based pseudo-LRU */
} CacheSet;

/*
 * Hash function for code object pointers.
 * Uses multiplicative hashing with golden ratio prime to spread aligned pointers.
 * Combines middle bits (more entropy than low bits for aligned pointers).
 */
static inline size_t cache_hash(uintptr_t addr) {
    /* Golden ratio prime for good distribution */
    const uintptr_t GOLDEN_RATIO = 0x9E3779B97F4A7C15ULL;
    
    /* Multiplicative hash - spreads aligned addresses across all buckets */
    uintptr_t h = addr * GOLDEN_RATIO;
    
    /* Use upper bits (better mixed by multiplication) */
    return (size_t)((h >> 32) ^ h) & CACHE_SET_MASK;
}

/*
 * Pseudo-LRU using a binary tree:
 *
 *        [bit 0]
 *        /     \
 *    [bit 1]  [bit 2]
 *     /  \     /  \
 *   W0   W1  W2   W3
 *
 * Bit meaning: 0 = go left, 1 = go right (for eviction victim selection)
 * On access: set bits to point AWAY from accessed way
 */

/* Get the eviction victim based on LRU bits */
static inline int lru_get_victim(uint8_t lru_bits) {
    if ((lru_bits & 0x1) == 0) {
        /* Go left */
        return ((lru_bits & 0x2) == 0) ? 0 : 1;
    } else {
        /* Go right */
        return ((lru_bits & 0x4) == 0) ? 2 : 3;
    }
}

/* Update LRU bits after accessing a way */
static inline uint8_t lru_update_access(uint8_t lru_bits, int way) {
    switch (way) {
        case 0:
            /* Accessed W0: point away (bit0=1, bit1=1) */
            return (lru_bits | 0x1) | 0x2;
        case 1:
            /* Accessed W1: point away (bit0=1, bit1=0) */
            return (lru_bits | 0x1) & ~0x2;
        case 2:
            /* Accessed W2: point away (bit0=0, bit2=1) */
            return (lru_bits & ~0x1) | 0x4;
        case 3:
            /* Accessed W3: point away (bit0=0, bit2=0) */
            return (lru_bits & ~0x1) & ~0x4;
        default:
            return lru_bits;
    }
}

/*
 * =============================================================================
 * Native Symbol Resolution (via dladdr)
 * =============================================================================
 */

#ifdef SPPROF_HAS_DLADDR

/* Python interpreter detection (more robust than string matching) */
static void* g_python_lib_base = NULL;   /* Base address of Python library */
static int g_python_base_initialized = 0;

/**
 * Initialize Python interpreter base address detection.
 *
 * Uses dladdr on a known Python C-API symbol to capture the base address
 * of the Python library. This is more robust than string matching on paths.
 *
 * Call once during resolver_init().
 */
static void init_python_interpreter_base(void) {
    if (g_python_base_initialized) {
        return;
    }
    
    Dl_info info;
    
    /* Use Py_Initialize as a well-known symbol that's always in the Python library.
     * We cast to void* to get the function address. */
    if (dladdr((void*)&Py_Initialize, &info) != 0 && info.dli_fbase != NULL) {
        g_python_lib_base = info.dli_fbase;
    }
    
    g_python_base_initialized = 1;
}

/**
 * Check if a frame address belongs to the Python interpreter.
 *
 * This is used by the "Trim & Sandwich" algorithm to determine where
 * to insert the Python stack within the native stack.
 *
 * Uses address-based detection: compares the library base address of the frame
 * against the known Python interpreter base address. Falls back to string
 * heuristics if address detection is unavailable.
 *
 * @param lib_base Base address of the library containing the frame (from dladdr).
 * @param lib_path Path to the library (used as fallback heuristic).
 * @return 1 if this is a Python interpreter frame, 0 otherwise.
 */
static int is_python_interpreter_frame(void* lib_base, const char* lib_path) {
    /* Primary method: compare library base addresses.
     * This is robust against renamed binaries, virtualenvs, etc. */
    if (g_python_lib_base != NULL && lib_base != NULL) {
        if (lib_base == g_python_lib_base) {
            return 1;
        }
        /* If we have a valid Python base but this frame is from a different
         * library, it's definitely not the interpreter. */
        return 0;
    }
    
    /* Fallback: string heuristics for when address detection failed.
     * This handles edge cases like dladdr returning NULL for some symbols. */
    if (lib_path == NULL) {
        return 0;
    }
    
    /* Check for Python framework (macOS) */
    if (strstr(lib_path, "Python.framework") != NULL) {
        return 1;
    }
    
    /* Check for libpython (Linux, Windows) */
    if (strstr(lib_path, "libpython") != NULL) {
        return 1;
    }
    
    /* Check for python executable itself */
    if (strstr(lib_path, "/python") != NULL || 
        strstr(lib_path, "\\python") != NULL) {
        return 1;
    }
    
    return 0;
}

/**
 * Legacy wrapper for backward compatibility.
 * Prefer is_python_interpreter_frame() which uses address-based detection.
 */
static int is_python_interpreter_library(const char* lib_path) {
    return is_python_interpreter_frame(NULL, lib_path);
}

/**
 * Resolve a native PC address to symbol information via dladdr.
 *
 * Safe to call after thread_resume() - this is the whole point of
 * deferring symbol resolution to the resolver.
 *
 * @param pc Raw instruction pointer (PC) from native stack walk.
 * @param out Output frame info with resolved symbol.
 * @param is_interpreter Output flag: 1 if this frame is in the Python interpreter.
 * @return 1 if resolved, 0 if dladdr failed.
 */
static int resolve_native_frame(uintptr_t pc, ResolvedFrame* out, int* is_interpreter) {
    Dl_info info;
    
    memset(out, 0, sizeof(*out));
    out->is_native = 1;
    out->lineno = 0;  /* Native frames don't have line numbers */
    
    if (is_interpreter) {
        *is_interpreter = 0;
    }
    
    if (pc == 0) {
        return 0;
    }
    
    /* Strip pointer authentication bits (Apple Silicon arm64e) */
#if defined(__arm64__) || defined(__aarch64__)
    /* Mask off PAC bits - they're in the upper bits */
    pc = pc & 0x0000007FFFFFFFFFULL;
#endif
    
    if (dladdr((void*)pc, &info) == 0) {
        /* dladdr failed - format as hex address */
        snprintf(out->function_name, SPPROF_MAX_FUNC_NAME, "0x%lx", (unsigned long)pc);
        out->filename[0] = '\0';
        return 0;
    }
    
    /* Get symbol name */
    if (info.dli_sname != NULL) {
        strncpy(out->function_name, info.dli_sname, SPPROF_MAX_FUNC_NAME - 1);
        out->function_name[SPPROF_MAX_FUNC_NAME - 1] = '\0';
    } else {
        /* No symbol - format as offset from nearest symbol base */
        if (info.dli_fname != NULL) {
            const char* basename = strrchr(info.dli_fname, '/');
            basename = basename ? basename + 1 : info.dli_fname;
            snprintf(out->function_name, SPPROF_MAX_FUNC_NAME, 
                     "%s+0x%lx", basename, 
                     (unsigned long)(pc - (uintptr_t)info.dli_fbase));
        } else {
            snprintf(out->function_name, SPPROF_MAX_FUNC_NAME, "0x%lx", (unsigned long)pc);
        }
    }
    
    /* Get library path */
    if (info.dli_fname != NULL) {
        strncpy(out->filename, info.dli_fname, SPPROF_MAX_FILENAME - 1);
        out->filename[SPPROF_MAX_FILENAME - 1] = '\0';
        
        /* Check if this is a Python interpreter frame using address-based detection */
        if (is_interpreter) {
            *is_interpreter = is_python_interpreter_frame(info.dli_fbase, info.dli_fname);
        }
    }
    
    return 1;
}

#else /* !SPPROF_HAS_DLADDR */

static int is_python_interpreter_library(const char* lib_path) {
    (void)lib_path;
    return 0;
}

static int resolve_native_frame(uintptr_t pc, ResolvedFrame* out, int* is_interpreter) {
    memset(out, 0, sizeof(*out));
    out->is_native = 1;
    out->lineno = 0;
    snprintf(out->function_name, SPPROF_MAX_FUNC_NAME, "0x%lx", (unsigned long)pc);
    if (is_interpreter) {
        *is_interpreter = 0;
    }
    return 0;
}

#endif /* SPPROF_HAS_DLADDR */

/*
 * =============================================================================
 * Mixed-Mode Frame Merging ("Trim & Sandwich" Algorithm)
 * =============================================================================
 *
 * The goal is to create a coherent stack trace that shows:
 *   [Native C functions (leaf)] -> [Python stack] -> [Native entry/main]
 *
 * Algorithm:
 *   1. Walk native stack from top (most recent/leaf frame)
 *   2. Add native frames until we hit the Python interpreter
 *   3. Insert ALL Python frames at the interpreter boundary
 *   4. Skip remaining interpreter frames in native stack
 *   5. Optionally add remaining native frames (main/entry points)
 */

/**
 * Merge native and Python frames using the "Trim & Sandwich" algorithm.
 *
 * @param native_pcs Array of native PC addresses (leaf first).
 * @param native_depth Number of native frames.
 * @param python_frames Array of Python code object pointers (leaf first).
 * @param instr_ptrs Array of instruction pointers for Python line numbers.
 * @param python_depth Number of Python frames.
 * @param out_frames Output array for merged frames.
 * @param max_frames Maximum frames to output.
 * @return Number of frames in merged output.
 */
static int merge_native_and_python_frames(
    const uintptr_t* native_pcs,
    int native_depth,
    const uintptr_t* python_frames,
    const uintptr_t* instr_ptrs,
    int python_depth,
    ResolvedFrame* out_frames,
    int max_frames
) {
    /* 
     * Debug assertions for parameter validation.
     * These help catch logic errors during development without runtime cost
     * in release builds (assert() compiles to nothing when NDEBUG is defined).
     */
    assert(native_depth >= 0 && "native_depth must be non-negative");
    assert(python_depth >= 0 && "python_depth must be non-negative");
    assert(max_frames > 0 && "max_frames must be positive");
    assert(out_frames != NULL && "out_frames must not be NULL");
    assert((native_depth == 0 || native_pcs != NULL) && "native_pcs required when native_depth > 0");
    assert((python_depth == 0 || python_frames != NULL) && "python_frames required when python_depth > 0");
    
    int out_idx = 0;
    int python_inserted = 0;
    
    /* If no native frames, just resolve Python frames */
    if (native_depth == 0) {
        for (int i = 0; i < python_depth && out_idx < max_frames; i++) {
            if (resolve_code_object_with_instr(python_frames[i], 
                                               instr_ptrs ? instr_ptrs[i] : 0,
                                               &out_frames[out_idx])) {
                out_idx++;
            }
        }
        return out_idx;
    }
    
    /* If no Python frames, just resolve native frames */
    if (python_depth == 0) {
        for (int i = 0; i < native_depth && out_idx < max_frames; i++) {
            int is_interp = 0;
            resolve_native_frame(native_pcs[i], &out_frames[out_idx], &is_interp);
            out_idx++;
        }
        return out_idx;
    }
    
    /* 
     * TRIM & SANDWICH MERGE:
     * Walk native stack from leaf (index 0) toward root.
     * Insert Python stack when we hit interpreter frames.
     */
    for (int i = 0; i < native_depth && out_idx < max_frames; i++) {
        int is_interp = 0;
        ResolvedFrame native_frame;
        
        resolve_native_frame(native_pcs[i], &native_frame, &is_interp);
        
        if (is_interp && !python_inserted) {
            /* We hit the interpreter - INSERT PYTHON STACK HERE */
            for (int j = 0; j < python_depth && out_idx < max_frames; j++) {
                if (resolve_code_object_with_instr(python_frames[j],
                                                   instr_ptrs ? instr_ptrs[j] : 0,
                                                   &out_frames[out_idx])) {
                    out_idx++;
                }
            }
            python_inserted = 1;
            
            /* Skip interpreter frames - we've replaced them with Python frames */
            /* Continue to add remaining non-interpreter frames (like main) */
        } else if (!is_interp) {
            /* Non-interpreter native frame - include it */
            out_frames[out_idx] = native_frame;
            out_idx++;
        }
        /* If is_interp && python_inserted, skip (don't duplicate interpreter frames) */
    }
    
    /* If we never hit interpreter frames, append Python stack at the end */
    if (!python_inserted) {
        for (int j = 0; j < python_depth && out_idx < max_frames; j++) {
            if (resolve_code_object_with_instr(python_frames[j],
                                               instr_ptrs ? instr_ptrs[j] : 0,
                                               &out_frames[out_idx])) {
                out_idx++;
            }
        }
    }
    
    return out_idx;
}

/* Global state */
static RingBuffer* g_ringbuffer = NULL;
static ResolvedSample* g_samples = NULL;
static size_t g_sample_count = 0;
static size_t g_sample_capacity = 0;
static CacheSet g_cache[CACHE_SETS];
static uint64_t g_cache_hits = 0;
static uint64_t g_cache_misses = 0;
static uint64_t g_cache_collisions = 0;  /* Track collision evictions */
static uint64_t g_invalid_frames = 0;
static int g_initialized = 0;

/*
 * Cache synchronization for thread safety.
 *
 * The symbol resolution cache is protected by a mutex to allow safe concurrent
 * access from multiple threads calling resolver_drain_samples() or
 * resolver_resolve_frame(). While the resolver was originally designed as
 * single-consumer, the streaming API (_drain_buffer) may be called from
 * multiple Python threads concurrently.
 *
 * The mutex protects:
 *   - g_cache array (read/write of cache entries and LRU bits)
 *   - g_cache_hits, g_cache_misses, g_cache_collisions counters
 *   - g_invalid_frames counter
 *
 * Note: The GIL is NOT sufficient protection because:
 *   1. Python releases the GIL during I/O and can yield mid-drain
 *   2. Multiple threads could call _drain_buffer() concurrently
 *   3. Future optimizations might add parallelism within the resolver
 */
#if defined(__APPLE__) || defined(__linux__)
static pthread_mutex_t g_cache_lock = PTHREAD_MUTEX_INITIALIZER;
#define CACHE_LOCK()   pthread_mutex_lock(&g_cache_lock)
#define CACHE_UNLOCK() pthread_mutex_unlock(&g_cache_lock)
#elif defined(_WIN32)
static CRITICAL_SECTION g_cache_lock;
static int g_cache_lock_initialized = 0;
#define CACHE_LOCK()   EnterCriticalSection(&g_cache_lock)
#define CACHE_UNLOCK() LeaveCriticalSection(&g_cache_lock)
#else
/* Fallback: no locking (single-threaded only) */
#define CACHE_LOCK()   ((void)0)
#define CACHE_UNLOCK() ((void)0)
#endif

/**
 * Compute line number from instruction pointer.
 *
 * Uses Python's internal line number table to map bytecode offset to line.
 * Requires the GIL to be held.
 *
 * On Windows:
 *   - If instr_ptr contains a line number directly (>0 and < 1M), use it
 *   - Otherwise fall back to co_firstlineno
 *   - The Windows platform now captures line numbers via PyFrame_GetLineNumber()
 *     and stores them in the instr_ptrs array
 *
 * On POSIX with Python 3.11+:
 *   - Use PyCode_Addr2Line for accurate line number from instruction pointer
 */
static int compute_lineno_from_instr(PyCodeObject* co, uintptr_t instr_ptr) {
    if (co == NULL) {
        return 0;
    }
    
    if (instr_ptr == 0) {
        return co->co_firstlineno;
    }

#if defined(_WIN32)
    /* On Windows, instr_ptr contains the line number directly (captured via
     * PyFrame_GetLineNumber in windows.c). We detect this by checking if it's
     * a reasonable line number (< 1,000,000). Actual instruction pointers would
     * be much larger memory addresses. */
    if (instr_ptr > 0 && instr_ptr < 1000000) {
        return (int)instr_ptr;
    }
    return co->co_firstlineno;
#elif PY_VERSION_HEX < 0x030B0000
    /* Python < 3.11: Use first line number */
    return co->co_firstlineno;
#else
    /* Python 3.11+ on POSIX: Use PyCode_Addr2Line if available */
    /* Calculate byte offset from code start */
    /* PyCode_GetCode returns a NEW REFERENCE - must decref! */
    PyObject* code_bytes = PyCode_GetCode((PyCodeObject*)co);
    if (code_bytes == NULL) {
        return co->co_firstlineno;
    }
    
    _Py_CODEUNIT* code_start = (_Py_CODEUNIT*)PyBytes_AS_STRING(code_bytes);
    if (code_start == NULL) {
        Py_DECREF(code_bytes);
        return co->co_firstlineno;
    }

    /* Compute offset in code units (2 bytes each) */
    Py_ssize_t offset = ((_Py_CODEUNIT*)instr_ptr - code_start);
    if (offset < 0) {
        Py_DECREF(code_bytes);
        return co->co_firstlineno;
    }

    /* Convert code unit offset to byte offset */
    int byte_offset = (int)(offset * sizeof(_Py_CODEUNIT));

    /* Use the address-to-line API */
    int lineno = PyCode_Addr2Line((PyCodeObject*)co, byte_offset);
    
    /* Release the code bytes reference */
    Py_DECREF(code_bytes);
    
    if (lineno < 0) {
        return co->co_firstlineno;
    }
    return lineno;
#endif
}

/* Resolve a code object to frame info */
static int resolve_code_object(uintptr_t code_addr, ResolvedFrame* out) {
    return resolve_code_object_with_instr(code_addr, 0, out);
}

/* Resolve a code object with instruction pointer for accurate line number */
static int resolve_code_object_with_instr(uintptr_t code_addr, uintptr_t instr_ptr, ResolvedFrame* out) {
    if (code_addr == 0) {
        return 0;
    }

    /* Need GIL to access Python objects */
    PyGILState_STATE gstate = PyGILState_Ensure();

    /*
     * SAFETY: Use code registry for validation before dereferencing.
     * 
     * This addresses the potential use-after-free issue where:
     * 1. Raw PyCodeObject* was captured by sampler
     * 2. GC ran between capture and now
     * 3. Code object was freed and memory possibly reused
     *
     * The registry validation checks:
     * - If we hold a reference (guaranteed valid)
     * - Basic pointer sanity (alignment, range)
     * - PyCode_Check (type verification)
     */
    CodeValidationResult validation = code_registry_validate(code_addr, 0);
    if (validation != CODE_VALID) {
        PyGILState_Release(gstate);
        return 0;
    }

    PyObject* code = (PyObject*)code_addr;

    PyCodeObject* co = (PyCodeObject*)code;

    /* Get function name */
    PyObject* name = co->co_name;
    if (name != NULL && PyUnicode_Check(name)) {
        const char* name_str = PyUnicode_AsUTF8(name);
        if (name_str != NULL) {
            strncpy(out->function_name, name_str, SPPROF_MAX_FUNC_NAME - 1);
            out->function_name[SPPROF_MAX_FUNC_NAME - 1] = '\0';
        } else {
            strcpy(out->function_name, "<unknown>");
        }
    } else {
        strcpy(out->function_name, "<unknown>");
    }

    /* Get filename */
    PyObject* filename = co->co_filename;
    if (filename != NULL && PyUnicode_Check(filename)) {
        const char* filename_str = PyUnicode_AsUTF8(filename);
        if (filename_str != NULL) {
            strncpy(out->filename, filename_str, SPPROF_MAX_FILENAME - 1);
            out->filename[SPPROF_MAX_FILENAME - 1] = '\0';
        } else {
            strcpy(out->filename, "<unknown>");
        }
    } else {
        strcpy(out->filename, "<unknown>");
    }

    /* Get line number - use instruction pointer if available for accuracy */
    if (instr_ptr != 0) {
        out->lineno = compute_lineno_from_instr(co, instr_ptr);
    } else {
        out->lineno = co->co_firstlineno;
    }
    out->is_native = 0;

    PyGILState_Release(gstate);
    return 1;
}

/* Cache lookup - searches all ways in the set (thread-safe) */
static int cache_lookup(uintptr_t code_addr, ResolvedFrame* out) {
    size_t set_idx = cache_hash(code_addr);
    int found = 0;
    
    CACHE_LOCK();
    
    CacheSet* set = &g_cache[set_idx];
    
    /* Search all ways for a match */
    for (int way = 0; way < CACHE_WAYS; way++) {
        CacheEntry* entry = &set->ways[way];
        if (entry->valid && entry->key == code_addr) {
            *out = entry->value;
            /* Update LRU - mark this way as most recently used */
            set->lru_bits = lru_update_access(set->lru_bits, way);
            g_cache_hits++;
            found = 1;
            break;
        }
    }
    
    if (!found) {
        g_cache_misses++;
    }
    
    CACHE_UNLOCK();
    return found;
}

/* Cache insert - uses LRU for eviction when set is full (thread-safe) */
static void cache_insert(uintptr_t code_addr, const ResolvedFrame* frame) {
    size_t set_idx = cache_hash(code_addr);
    
    CACHE_LOCK();
    
    CacheSet* set = &g_cache[set_idx];
    
    /* First, check if key already exists (update in place) */
    for (int way = 0; way < CACHE_WAYS; way++) {
        CacheEntry* entry = &set->ways[way];
        if (entry->valid && entry->key == code_addr) {
            entry->value = *frame;
            set->lru_bits = lru_update_access(set->lru_bits, way);
            CACHE_UNLOCK();
            return;
        }
    }
    
    /* Second, look for an empty slot */
    for (int way = 0; way < CACHE_WAYS; way++) {
        CacheEntry* entry = &set->ways[way];
        if (!entry->valid) {
            entry->key = code_addr;
            entry->value = *frame;
            entry->valid = 1;
            set->lru_bits = lru_update_access(set->lru_bits, way);
            CACHE_UNLOCK();
            return;
        }
    }
    
    /* Set is full - evict LRU victim */
    int victim = lru_get_victim(set->lru_bits);
    CacheEntry* entry = &set->ways[victim];
    
    if (entry->valid) {
        g_cache_collisions++;  /* Track actual evictions */
    }
    
    entry->key = code_addr;
    entry->value = *frame;
    entry->valid = 1;
    set->lru_bits = lru_update_access(set->lru_bits, victim);
    
    CACHE_UNLOCK();
}

int resolver_init(RingBuffer* rb) {
    if (g_initialized) {
        return 0;
    }

#ifdef _WIN32
    /* Initialize Windows critical section for cache synchronization */
    if (!g_cache_lock_initialized) {
        InitializeCriticalSection(&g_cache_lock);
        g_cache_lock_initialized = 1;
    }
#endif

    g_ringbuffer = rb;
    g_sample_count = 0;
    g_sample_capacity = 1024;
    g_samples = (ResolvedSample*)calloc(g_sample_capacity, sizeof(ResolvedSample));
    if (g_samples == NULL) {
        return -1;
    }

    /* Clear cache (sets all entries invalid and LRU bits to 0) */
    CACHE_LOCK();
    memset(g_cache, 0, sizeof(g_cache));
    g_cache_hits = 0;
    g_cache_misses = 0;
    g_cache_collisions = 0;
    g_invalid_frames = 0;
    CACHE_UNLOCK();

    /* Initialize code object registry for safe reference tracking */
    if (code_registry_init() != 0) {
        free(g_samples);
        g_samples = NULL;
        return -1;
    }

#ifdef SPPROF_HAS_DLADDR
    /* Initialize Python interpreter base address for robust detection */
    init_python_interpreter_base();
#endif

    g_initialized = 1;
    return 0;
}

void resolver_shutdown(void) {
    if (!g_initialized) {
        return;
    }

    /* Clean up code registry (releases held references) */
    code_registry_cleanup();

    /* Free samples if not already freed */
    if (g_samples != NULL) {
        free(g_samples);
        g_samples = NULL;
        g_sample_count = 0;
        g_sample_capacity = 0;
    }
    
    /* Clear cache (protected by mutex) */
    CACHE_LOCK();
    memset(g_cache, 0, sizeof(g_cache));
    CACHE_UNLOCK();
    
    /* Note: We don't reset g_python_lib_base or g_python_base_initialized here.
     * The Python interpreter base address doesn't change during process lifetime,
     * so there's no need to re-detect it on each profiler restart. */
    
    /* Note: We don't destroy the cache mutex/critical section here because
     * it may be reused if profiling is restarted. On POSIX, the static
     * PTHREAD_MUTEX_INITIALIZER doesn't need destruction. On Windows, the
     * CRITICAL_SECTION persists for process lifetime. */
    
    g_ringbuffer = NULL;
    g_initialized = 0;
}

/**
 * Resolve a single raw sample into a resolved sample.
 *
 * This is the common resolution logic shared by resolver_get_samples() and
 * resolver_drain_samples(). Extracts the duplicated sample processing code
 * into a reusable helper.
 *
 * @param raw Input raw sample with unresolved frame pointers.
 * @param out Output resolved sample with function names and filenames.
 * @return 1 if sample has at least one resolved frame, 0 otherwise.
 *
 * SIDE EFFECTS:
 *   - Releases code object references via code_registry_release_refs_batch()
 *   - Increments g_invalid_frames counter for unresolvable frames
 *   - May update resolution cache
 */
static int resolve_raw_sample(const RawSample* raw, ResolvedSample* out) {
    out->timestamp = raw->timestamp;
    out->thread_id = raw->thread_id;
    out->depth = 0;

    /*
     * MIXED-MODE RESOLUTION:
     * If we have native frames, use the "Trim & Sandwich" merge algorithm.
     * This produces a coherent stack: [Native C] -> [Python] -> [Entry]
     */
    if (raw->native_depth > 0) {
        /* Use merge algorithm for mixed-mode samples */
        out->depth = merge_native_and_python_frames(
            raw->native_pcs,
            raw->native_depth,
            raw->frames,
            raw->instr_ptrs,
            raw->depth,
            out->frames,
            SPPROF_MAX_STACK_DEPTH
        );
    } else {
        /* Python-only sample (legacy path) */
        for (int i = 0; i < raw->depth && i < SPPROF_MAX_STACK_DEPTH; i++) {
            uintptr_t code_addr = raw->frames[i];
            uintptr_t instr_ptr = raw->instr_ptrs[i];
            ResolvedFrame* frame = &out->frames[out->depth];

            /* If we have an instruction pointer, resolve with it for accuracy */
            /* Cache is only used for code_addr (lineno may vary per call site) */
            if (instr_ptr != 0) {
                /* Resolve with instruction pointer - don't cache as line varies */
                if (resolve_code_object_with_instr(code_addr, instr_ptr, frame)) {
                    out->depth++;
                } else {
                    g_invalid_frames++;
                }
            } else {
                /* No instruction pointer - use cache */
                if (cache_lookup(code_addr, frame)) {
                    out->depth++;
                    continue;
                }

                /* Resolve and cache */
                if (resolve_code_object(code_addr, frame)) {
                    cache_insert(code_addr, frame);
                    out->depth++;
                } else {
                    g_invalid_frames++;
                }
            }
        }
    }

    /*
     * SAFETY: Release code object references after processing sample.
     *
     * The Darwin/Mach sampler adds refs via code_registry_add_refs_batch()
     * during capture. Now that we've resolved the frames, release those refs.
     * For samples from signal handlers (Linux), this is a no-op since they
     * don't add refs (can't call Python API from signal handler).
     */
    if (raw->depth > 0) {
        code_registry_release_refs_batch(raw->frames, raw->depth);
    }

    return out->depth > 0 ? 1 : 0;
}

int resolver_get_samples(ResolvedSample** out, size_t* count) {
    if (g_ringbuffer == NULL) {
        *out = NULL;
        *count = 0;
        return 0;
    }

    /* Drain remaining samples from ring buffer */
    RawSample raw;
    while (ringbuffer_read(g_ringbuffer, &raw)) {
        /* Expand array if needed */
        if (g_sample_count >= g_sample_capacity) {
            size_t new_capacity = g_sample_capacity * 2;
            ResolvedSample* new_samples = (ResolvedSample*)realloc(
                g_samples, new_capacity * sizeof(ResolvedSample));
            if (new_samples == NULL) {
                return -1;
            }
            g_samples = new_samples;
            g_sample_capacity = new_capacity;
        }

        ResolvedSample* sample = &g_samples[g_sample_count];
        if (resolve_raw_sample(&raw, sample)) {
            g_sample_count++;
        }
    }

    *out = g_samples;
    *count = g_sample_count;
    return 0;
}

void resolver_free_samples(ResolvedSample* samples, size_t count) {
    /* Only free if it matches our global samples array (avoid double free) */
    if (samples != NULL && samples == g_samples) {
        free(samples);
        g_samples = NULL;
        g_sample_count = 0;
        g_sample_capacity = 0;
    }
    (void)count;  /* Unused parameter */
}

int resolver_resolve_frame(uintptr_t code_addr, ResolvedFrame* out) {
    if (cache_lookup(code_addr, out)) {
        return 1;
    }

    if (resolve_code_object(code_addr, out)) {
        cache_insert(code_addr, out);
        return 1;
    }

    return 0;
}

int resolver_resolve_frame_with_line(uintptr_t code_addr, uintptr_t instr_ptr, ResolvedFrame* out) {
    /* With instruction pointer, we need to resolve for accurate line number */
    if (instr_ptr != 0) {
        return resolve_code_object_with_instr(code_addr, instr_ptr, out);
    }

    /* Fall back to cached resolution */
    return resolver_resolve_frame(code_addr, out);
}

void resolver_clear_cache(void) {
    CACHE_LOCK();
    memset(g_cache, 0, sizeof(g_cache));
    g_cache_hits = 0;
    g_cache_misses = 0;
    g_cache_collisions = 0;
    CACHE_UNLOCK();
}

void resolver_get_stats(uint64_t* cache_hits, uint64_t* cache_misses, 
                        uint64_t* cache_collisions, uint64_t* invalid_frames) {
    CACHE_LOCK();
    if (cache_hits) *cache_hits = g_cache_hits;
    if (cache_misses) *cache_misses = g_cache_misses;
    if (cache_collisions) *cache_collisions = g_cache_collisions;
    if (invalid_frames) *invalid_frames = g_invalid_frames;
    CACHE_UNLOCK();
}

int resolver_has_pending_samples(void) {
    if (g_ringbuffer == NULL) {
        return 0;
    }
    return ringbuffer_has_data(g_ringbuffer);
}

int resolver_drain_samples(size_t max_samples, ResolvedSample** out, size_t* count) {
    *out = NULL;
    *count = 0;
    
    if (g_ringbuffer == NULL) {
        return 0;
    }
    
    /* Default chunk size if max_samples is 0 */
    if (max_samples == 0) {
        max_samples = 10000;  /* Reasonable default batch size */
    }
    
    /* Allocate output array for this batch */
    ResolvedSample* samples = (ResolvedSample*)calloc(max_samples, sizeof(ResolvedSample));
    if (samples == NULL) {
        return -1;
    }
    
    size_t sample_count = 0;
    RawSample raw;
    
    while (sample_count < max_samples && ringbuffer_read(g_ringbuffer, &raw)) {
        ResolvedSample* sample = &samples[sample_count];
        if (resolve_raw_sample(&raw, sample)) {
            sample_count++;
        }
    }
    
    /* Shrink allocation if we got fewer samples than max */
    if (sample_count > 0 && sample_count < max_samples) {
        ResolvedSample* shrunk = (ResolvedSample*)realloc(samples, 
                                                           sample_count * sizeof(ResolvedSample));
        if (shrunk != NULL) {
            samples = shrunk;
        }
        /* If realloc fails, original allocation is still valid */
    } else if (sample_count == 0) {
        free(samples);
        samples = NULL;
    }
    
    *out = samples;
    *count = sample_count;
    return 0;
}


