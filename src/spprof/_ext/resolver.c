/**
 * resolver.c - Symbol resolution from raw frame pointers
 *
 * The resolver runs in a background thread, consuming samples from
 * the ring buffer and resolving PyCodeObject* pointers to readable
 * function names, filenames, and line numbers.
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <string.h>
#include <stdlib.h>

#include "resolver.h"

/* Simple LRU cache for resolved frames */
#define CACHE_SIZE 4096
#define CACHE_MASK (CACHE_SIZE - 1)

/* Forward declarations */
static int resolve_code_object_with_instr(uintptr_t code_addr, uintptr_t instr_ptr, ResolvedFrame* out);

typedef struct {
    uintptr_t key;
    ResolvedFrame value;
    int valid;
} CacheEntry;

/* Global state */
static RingBuffer* g_ringbuffer = NULL;
static ResolvedSample* g_samples = NULL;
static size_t g_sample_count = 0;
static size_t g_sample_capacity = 0;
static CacheEntry g_cache[CACHE_SIZE];
static uint64_t g_cache_hits = 0;
static uint64_t g_cache_misses = 0;
static uint64_t g_invalid_frames = 0;
static int g_initialized = 0;

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

    PyObject* code = (PyObject*)code_addr;

    /* Verify it's a code object */
    if (!PyCode_Check(code)) {
        PyGILState_Release(gstate);
        return 0;
    }

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

/* Cache lookup */
static int cache_lookup(uintptr_t code_addr, ResolvedFrame* out) {
    size_t idx = (size_t)(code_addr & CACHE_MASK);
    CacheEntry* entry = &g_cache[idx];

    if (entry->valid && entry->key == code_addr) {
        *out = entry->value;
        g_cache_hits++;
        return 1;
    }

    g_cache_misses++;
    return 0;
}

/* Cache insert */
static void cache_insert(uintptr_t code_addr, const ResolvedFrame* frame) {
    size_t idx = (size_t)(code_addr & CACHE_MASK);
    CacheEntry* entry = &g_cache[idx];

    entry->key = code_addr;
    entry->value = *frame;
    entry->valid = 1;
}

int resolver_init(RingBuffer* rb) {
    if (g_initialized) {
        return 0;
    }

    g_ringbuffer = rb;
    g_sample_count = 0;
    g_sample_capacity = 1024;
    g_samples = (ResolvedSample*)calloc(g_sample_capacity, sizeof(ResolvedSample));
    if (g_samples == NULL) {
        return -1;
    }

    /* Clear cache */
    memset(g_cache, 0, sizeof(g_cache));
    g_cache_hits = 0;
    g_cache_misses = 0;
    g_invalid_frames = 0;

    g_initialized = 1;
    return 0;
}

void resolver_shutdown(void) {
    if (!g_initialized) {
        return;
    }

    /* Free samples if not already freed */
    if (g_samples != NULL) {
        free(g_samples);
        g_samples = NULL;
        g_sample_count = 0;
        g_sample_capacity = 0;
    }
    
    /* Clear cache */
    memset(g_cache, 0, sizeof(g_cache));
    
    g_ringbuffer = NULL;
    g_initialized = 0;
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
        sample->timestamp = raw.timestamp;
        sample->thread_id = raw.thread_id;
        sample->depth = 0;

        /* Resolve each frame with instruction pointer for accurate line numbers */
        for (int i = 0; i < raw.depth && i < SPPROF_MAX_STACK_DEPTH; i++) {
            uintptr_t code_addr = raw.frames[i];
            uintptr_t instr_ptr = raw.instr_ptrs[i];
            ResolvedFrame* frame = &sample->frames[sample->depth];

            /* If we have an instruction pointer, resolve with it for accuracy */
            /* Cache is only used for code_addr (lineno may vary per call site) */
            if (instr_ptr != 0) {
                /* Resolve with instruction pointer - don't cache as line varies */
                if (resolve_code_object_with_instr(code_addr, instr_ptr, frame)) {
                    sample->depth++;
                } else {
                    g_invalid_frames++;
                }
            } else {
                /* No instruction pointer - use cache */
                if (cache_lookup(code_addr, frame)) {
                    sample->depth++;
                    continue;
                }

                /* Resolve and cache */
                if (resolve_code_object(code_addr, frame)) {
                    cache_insert(code_addr, frame);
                    sample->depth++;
                } else {
                    g_invalid_frames++;
                }
            }
        }

        if (sample->depth > 0) {
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
    memset(g_cache, 0, sizeof(g_cache));
    g_cache_hits = 0;
    g_cache_misses = 0;
}

void resolver_get_stats(uint64_t* cache_hits, uint64_t* cache_misses, uint64_t* invalid_frames) {
    *cache_hits = g_cache_hits;
    *cache_misses = g_cache_misses;
    *invalid_frames = g_invalid_frames;
}


