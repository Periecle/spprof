/**
 * code_registry.c - Safe code object reference tracking implementation
 *
 * This module uses a hash table to track code objects that have been
 * captured but not yet resolved. When the GIL is held during capture
 * (Darwin/Mach sampler), we INCREF the code objects to prevent GC.
 *
 * For signal-handler captured samples (Linux), we can't INCREF, so we
 * track the GC epoch and validate at resolution time.
 *
 * Copyright (c) 2024 spprof contributors
 * SPDX-License-Identifier: MIT
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <string.h>
#include <stdlib.h>

#include "code_registry.h"
#include "uthash.h"  /* Header-only hash table */

/*
 * =============================================================================
 * Data Structures
 * =============================================================================
 */

/**
 * Hash table entry for tracking code object references.
 */
typedef struct {
    uintptr_t code_addr;        /* Key: code object address */
    uint32_t refcount;          /* Number of samples referencing this code */
    uint64_t capture_epoch;     /* GC epoch when first captured */
    int has_python_ref;         /* 1 if we hold a Python reference (INCREF'd) */
    UT_hash_handle hh;          /* uthash handle */
} CodeEntry;

/*
 * =============================================================================
 * Global State
 * =============================================================================
 */

/* Hash table of tracked code objects */
static CodeEntry* g_code_table = NULL;

/* Statistics */
static uint64_t g_refs_added = 0;
static uint64_t g_refs_released = 0;
static uint64_t g_validations = 0;
static uint64_t g_invalid_count = 0;
static uint64_t g_safe_mode_rejects = 0;

/* Initialization flag */
static int g_initialized = 0;

/* Safe mode flag - when enabled, reject unregistered code pointers */
static int g_safe_mode = 0;

/*
 * =============================================================================
 * GC Epoch Tracking
 * =============================================================================
 */

/**
 * Get the current GC epoch (sum of all generation collection counts).
 *
 * This requires the GIL and uses the gc module.
 */
uint64_t code_registry_get_gc_epoch(void) {
    uint64_t epoch = 0;
    
    /* Import gc module */
    PyObject* gc_module = PyImport_ImportModule("gc");
    if (gc_module == NULL) {
        PyErr_Clear();
        return 0;
    }
    
    /* Call gc.get_count() to get collection counts for each generation */
    PyObject* counts = PyObject_CallMethod(gc_module, "get_count", NULL);
    if (counts != NULL && PyTuple_Check(counts)) {
        Py_ssize_t n = PyTuple_Size(counts);
        for (Py_ssize_t i = 0; i < n; i++) {
            PyObject* item = PyTuple_GetItem(counts, i);
            if (item != NULL && PyLong_Check(item)) {
                epoch += PyLong_AsUnsignedLongLong(item);
            }
        }
        Py_DECREF(counts);
    } else {
        PyErr_Clear();
    }
    
    Py_DECREF(gc_module);
    return epoch;
}

/*
 * =============================================================================
 * Initialization / Cleanup
 * =============================================================================
 */

int code_registry_init(void) {
    if (g_initialized) {
        return 0;  /* Already initialized */
    }
    
    g_code_table = NULL;
    g_refs_added = 0;
    g_refs_released = 0;
    g_validations = 0;
    g_invalid_count = 0;
    g_safe_mode_rejects = 0;
    /* Note: g_safe_mode is preserved across init/cleanup cycles
     * to maintain user configuration. Reset explicitly if needed. */
    g_initialized = 1;
    
    return 0;
}

void code_registry_cleanup(void) {
    if (!g_initialized) {
        return;
    }
    
    /* Release all held references */
    code_registry_clear_all();
    
    g_initialized = 0;
}

void code_registry_clear_all(void) {
    if (!g_initialized) {
        return;
    }
    
    /* Need GIL to DECREF */
    PyGILState_STATE gstate = PyGILState_Ensure();
    
    CodeEntry* entry;
    CodeEntry* tmp;
    
    HASH_ITER(hh, g_code_table, entry, tmp) {
        HASH_DEL(g_code_table, entry);
        
        /* Release Python reference if we hold one */
        if (entry->has_python_ref && entry->code_addr != 0) {
            PyObject* obj = (PyObject*)entry->code_addr;
            /* Verify it's still a code object before DECREF */
            if (PyCode_Check(obj)) {
                Py_DECREF(obj);
            }
        }
        
        free(entry);
    }
    
    g_code_table = NULL;
    
    PyGILState_Release(gstate);
}

/*
 * =============================================================================
 * Reference Management
 * =============================================================================
 */

int code_registry_add_ref(uintptr_t code_addr, uint64_t gc_epoch) {
    if (!g_initialized || code_addr == 0) {
        return 0;
    }
    
    /* Check if already tracked */
    CodeEntry* entry = NULL;
    HASH_FIND(hh, g_code_table, &code_addr, sizeof(code_addr), entry);
    
    if (entry != NULL) {
        /* Already tracking - increment count */
        entry->refcount++;
        g_refs_added++;
        return 1;
    }
    
    /* Validate it's actually a code object before INCREF */
    PyObject* obj = (PyObject*)code_addr;
    if (!PyCode_Check(obj)) {
        return 0;
    }
    
    /* Create new entry */
    entry = (CodeEntry*)malloc(sizeof(CodeEntry));
    if (entry == NULL) {
        return 0;
    }
    
    entry->code_addr = code_addr;
    entry->refcount = 1;
    entry->capture_epoch = gc_epoch;
    entry->has_python_ref = 1;
    
    /* INCREF to hold reference */
    Py_INCREF(obj);
    
    /* Add to hash table */
    HASH_ADD(hh, g_code_table, code_addr, sizeof(code_addr), entry);
    
    g_refs_added++;
    return 1;
}

int code_registry_add_refs_batch(uintptr_t* code_addrs, size_t count, uint64_t gc_epoch) {
    if (!g_initialized || code_addrs == NULL || count == 0) {
        return 0;
    }
    
    int added = 0;
    for (size_t i = 0; i < count; i++) {
        if (code_registry_add_ref(code_addrs[i], gc_epoch)) {
            added++;
        }
    }
    
    return added;
}

void code_registry_release_ref(uintptr_t code_addr) {
    if (!g_initialized || code_addr == 0) {
        return;
    }
    
    CodeEntry* entry = NULL;
    HASH_FIND(hh, g_code_table, &code_addr, sizeof(code_addr), entry);
    
    if (entry == NULL) {
        return;  /* Not tracked */
    }
    
    entry->refcount--;
    g_refs_released++;
    
    if (entry->refcount == 0) {
        /* Remove from table */
        HASH_DEL(g_code_table, entry);
        
        /* Release Python reference */
        if (entry->has_python_ref) {
            PyObject* obj = (PyObject*)code_addr;
            /* The object should still be valid since we hold a reference */
            Py_DECREF(obj);
        }
        
        free(entry);
    }
}

void code_registry_release_refs_batch(uintptr_t* code_addrs, size_t count) {
    if (!g_initialized || code_addrs == NULL || count == 0) {
        return;
    }
    
    for (size_t i = 0; i < count; i++) {
        code_registry_release_ref(code_addrs[i]);
    }
}

/*
 * =============================================================================
 * Validation
 * =============================================================================
 */

/**
 * Basic pointer validation.
 *
 * Check if a pointer looks valid (non-null, aligned).
 */
static int is_pointer_valid(uintptr_t addr) {
    /* NULL check */
    if (addr == 0) {
        return 0;
    }
    
    /* Alignment check - Python objects are at least 8-byte aligned */
    if ((addr & 0x7) != 0) {
        return 0;
    }
    
    /* Very low addresses are typically not valid user-space pointers */
    if (addr < 0x1000) {
        return 0;
    }
    
    return 1;
}

CodeValidationResult code_registry_validate(uintptr_t code_addr, uint64_t capture_epoch) {
    g_validations++;
    
    /* Basic pointer validation */
    if (!is_pointer_valid(code_addr)) {
        g_invalid_count++;
        return CODE_INVALID_NULL;
    }
    
    /* If we're tracking this object, it's guaranteed valid */
    CodeEntry* entry = NULL;
    HASH_FIND(hh, g_code_table, &code_addr, sizeof(code_addr), entry);
    
    if (entry != NULL && entry->has_python_ref) {
        /* We hold a reference, so it's valid */
        return CODE_VALID;
    }
    
    /*
     * SAFE MODE CHECK:
     *
     * If safe mode is enabled, reject any code objects not held by the registry.
     * This addresses the potential race condition in signal-handler captured
     * samples (Linux): between capture and resolution, GC could free the code
     * object, and the memory could be reused.
     *
     * Trade-off:
     *   - Disabled: Process all samples, accept tiny theoretical risk
     *   - Enabled: Discard unregistered samples for guaranteed safety
     *
     * This is a production safety feature. Darwin/Mach samples are always
     * held (INCREF'd during capture), so this only affects Linux signal-handler
     * samples where we cannot INCREF in async-signal-safe context.
     */
    if (g_safe_mode) {
        g_safe_mode_rejects++;
        return CODE_INVALID_NOT_HELD;
    }
    
    /* Check GC epoch if provided */
    if (capture_epoch != 0) {
        uint64_t current_epoch = code_registry_get_gc_epoch();
        if (current_epoch != capture_epoch) {
            /* GC has run since capture - object may have been freed.
             * We still try to validate via PyCode_Check, but this is
             * a warning that extra caution is needed. */
            /* Note: We don't return CODE_INVALID_GC_STALE here because
             * the object may still be valid. We just note that GC ran. */
        }
    }
    
    /* Type check via PyCode_Check */
    PyObject* obj = (PyObject*)code_addr;
    
    /* PyCode_Check is safe to call here because:
     * 1. We have the GIL (required by code_registry_validate)
     * 2. The pointer passed basic validation
     * 
     * In the worst case (freed memory reused), PyCode_Check reads the
     * object's type pointer. If it happens to match PyCode_Type, we
     * might get a false positive, but this is extremely rare.
     * 
     * If the memory is completely unmapped, this could SEGFAULT.
     * On systems with memory protection, this should not happen as
     * Python's allocator keeps freed memory in pools.
     */
    if (!PyCode_Check(obj)) {
        g_invalid_count++;
        return CODE_INVALID_TYPE;
    }
    
    return CODE_VALID;
}

int code_registry_is_held(uintptr_t code_addr) {
    if (!g_initialized || code_addr == 0) {
        return 0;
    }
    
    CodeEntry* entry = NULL;
    HASH_FIND(hh, g_code_table, &code_addr, sizeof(code_addr), entry);
    
    return (entry != NULL && entry->has_python_ref);
}

/*
 * =============================================================================
 * Statistics
 * =============================================================================
 */

void code_registry_get_stats(
    uint64_t* refs_held,
    uint64_t* refs_added,
    uint64_t* refs_released,
    uint64_t* validations,
    uint64_t* invalid_count
) {
    if (refs_held) {
        *refs_held = HASH_COUNT(g_code_table);
    }
    if (refs_added) {
        *refs_added = g_refs_added;
    }
    if (refs_released) {
        *refs_released = g_refs_released;
    }
    if (validations) {
        *validations = g_validations;
    }
    if (invalid_count) {
        *invalid_count = g_invalid_count;
    }
}

void code_registry_reset_stats(void) {
    g_refs_added = 0;
    g_refs_released = 0;
    g_validations = 0;
    g_invalid_count = 0;
    g_safe_mode_rejects = 0;
}

/*
 * =============================================================================
 * Safe Mode
 * =============================================================================
 */

void code_registry_set_safe_mode(int enabled) {
    g_safe_mode = enabled ? 1 : 0;
}

int code_registry_is_safe_mode(void) {
    return g_safe_mode;
}

void code_registry_get_stats_extended(
    uint64_t* refs_held,
    uint64_t* refs_added,
    uint64_t* refs_released,
    uint64_t* validations,
    uint64_t* invalid_count,
    uint64_t* safe_mode_rejects
) {
    if (refs_held) {
        *refs_held = HASH_COUNT(g_code_table);
    }
    if (refs_added) {
        *refs_added = g_refs_added;
    }
    if (refs_released) {
        *refs_released = g_refs_released;
    }
    if (validations) {
        *validations = g_validations;
    }
    if (invalid_count) {
        *invalid_count = g_invalid_count;
    }
    if (safe_mode_rejects) {
        *safe_mode_rejects = g_safe_mode_rejects;
    }
}

