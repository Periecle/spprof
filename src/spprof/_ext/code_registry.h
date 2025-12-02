/**
 * code_registry.h - Safe code object reference tracking
 *
 * This module addresses the potential use-after-free issue when raw
 * PyCodeObject* pointers are captured by the sampler and later resolved.
 *
 * Problem:
 *   1. Sampler captures raw PyCodeObject* pointers
 *   2. Between capture and resolution, GC might run and free the code object
 *   3. Resolver tries to dereference freed memory → crash/corruption
 *
 * Solution:
 *   - For Darwin/Mach sampler (GIL held during capture):
 *     INCREF code objects at capture time, DECREF after resolution
 *   - For signal handler (no GIL):
 *     Track pointers and validate at resolution time using GC epoch
 *   - Add safe memory validation before PyCode_Check
 *
 * Usage:
 *   1. Call code_registry_init() at profiler startup
 *   2. When capturing (with GIL): code_registry_add_ref() for each code object
 *   3. When resolving: code_registry_validate() before accessing
 *   4. After resolving: code_registry_release_ref() to decrement
 *   5. Call code_registry_cleanup() at profiler shutdown
 *
 * ERROR HANDLING CONVENTIONS (see error.h for full documentation):
 *   - Lifecycle functions (init/cleanup): Return 0 on success, -1 on error
 *   - Try operations (add_ref): Return 1 on success, 0 on failure (boolean)
 *   - Validation: Return CodeValidationResult enum for rich error info
 *   - Query functions (is_held): Return 1 for true, 0 for false (boolean)
 *
 * Copyright (c) 2024 spprof contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef SPPROF_CODE_REGISTRY_H
#define SPPROF_CODE_REGISTRY_H

#include <Python.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * CodeValidationResult - Validation result for code objects.
 *
 * This enum provides detailed error information for code pointer validation.
 * Use code_validation_succeeded() to check for success.
 *
 * Design note: Uses enum pattern (Pattern 3 from error.h) because validation
 * can fail in multiple distinct ways that callers may want to handle differently.
 */
typedef enum {
    CODE_VALID = 0,           /* Code object is valid and safe to use */
    CODE_INVALID_NULL,        /* NULL pointer */
    CODE_INVALID_FREED,       /* Memory appears to be freed/corrupted */
    CODE_INVALID_TYPE,        /* Not a PyCodeObject (PyCode_Check failed) */
    CODE_INVALID_GC_STALE,    /* GC ran since capture, may be invalid */
    CODE_INVALID_NOT_HELD,    /* Not held by registry, discarded in safe mode */
} CodeValidationResult;

/**
 * Check if a CodeValidationResult indicates success.
 *
 * @param result The validation result to check.
 * @return 1 if valid (CODE_VALID), 0 otherwise.
 */
static inline int code_validation_succeeded(CodeValidationResult result) {
    return result == CODE_VALID;
}

/**
 * Convert CodeValidationResult to human-readable string.
 *
 * @param result The validation result to convert.
 * @return Static string describing the result.
 */
static inline const char* code_validation_str(CodeValidationResult result) {
    switch (result) {
        case CODE_VALID:           return "valid";
        case CODE_INVALID_NULL:    return "null pointer";
        case CODE_INVALID_FREED:   return "memory freed or corrupted";
        case CODE_INVALID_TYPE:    return "not a code object (PyCode_Check failed)";
        case CODE_INVALID_GC_STALE: return "GC ran since capture, may be invalid";
        case CODE_INVALID_NOT_HELD: return "not held by registry (safe mode)";
        default:                    return "unknown validation result";
    }
}

/**
 * Initialize the code object registry.
 *
 * Call once at profiler startup (before sampling begins).
 *
 * Error handling: POSIX-style (Pattern 1 from error.h)
 *
 * @return 0 on success, -1 on error (memory allocation failure).
 */
int code_registry_init(void);

/**
 * Clean up the code object registry.
 *
 * Releases any held references and frees resources.
 * Call at profiler shutdown.
 */
void code_registry_cleanup(void);

/**
 * Add a reference to a code object (REQUIRES GIL).
 *
 * Call this when capturing a code object pointer while holding the GIL.
 * This increments the reference count to prevent the object from being
 * garbage collected before resolution.
 *
 * Thread safety: Requires GIL.
 * Async-signal safety: NO.
 *
 * Error handling: Boolean success (Pattern 2 from error.h)
 *   Returns 1 = success (reference added)
 *   Returns 0 = failure (NULL pointer, not a code object, or not initialized)
 *
 * @param code_addr Raw PyCodeObject* pointer.
 * @param gc_epoch  Current GC generation (from code_registry_get_gc_epoch()).
 * @return 1 if reference added successfully, 0 on failure.
 */
int code_registry_add_ref(uintptr_t code_addr, uint64_t gc_epoch);

/**
 * Add multiple code object references at once (REQUIRES GIL).
 *
 * More efficient than multiple calls to code_registry_add_ref() when
 * capturing an entire stack.
 *
 * @param code_addrs Array of raw PyCodeObject* pointers.
 * @param count      Number of pointers in the array.
 * @param gc_epoch   Current GC generation.
 * @return Number of references successfully added.
 */
int code_registry_add_refs_batch(uintptr_t* code_addrs, size_t count, uint64_t gc_epoch);

/**
 * Release a reference to a code object (REQUIRES GIL).
 *
 * Call this after resolving a code object to decrement the reference count.
 *
 * Thread safety: Requires GIL.
 * Async-signal safety: NO.
 *
 * @param code_addr Raw PyCodeObject* pointer.
 */
void code_registry_release_ref(uintptr_t code_addr);

/**
 * Release multiple code object references at once (REQUIRES GIL).
 *
 * @param code_addrs Array of raw PyCodeObject* pointers.
 * @param count      Number of pointers in the array.
 */
void code_registry_release_refs_batch(const uintptr_t* code_addrs, size_t count);

/**
 * Validate a code object pointer (REQUIRES GIL).
 *
 * Performs multiple safety checks:
 *   1. NULL check
 *   2. Memory alignment check
 *   3. PyCode_Check (type validation)
 *   4. GC epoch comparison (if capture_epoch provided)
 *
 * If the code object was registered with code_registry_add_ref(), it is
 * guaranteed to be valid (reference count prevents GC).
 *
 * Thread safety: Requires GIL.
 * Async-signal safety: NO.
 *
 * Error handling: Enum result (Pattern 3 from error.h)
 *   Use code_validation_succeeded() to check for success.
 *   Use code_validation_str() to get human-readable error message.
 *
 * @param code_addr     Raw PyCodeObject* pointer.
 * @param capture_epoch GC epoch when pointer was captured (0 to skip check).
 * @return CodeValidationResult - CODE_VALID on success, error code otherwise.
 */
CodeValidationResult code_registry_validate(uintptr_t code_addr, uint64_t capture_epoch);

/**
 * Check if a code object is currently held by the registry.
 *
 * If true, the code object is guaranteed valid (reference held).
 *
 * Thread safety: Requires GIL.
 *
 * Error handling: Boolean query (Pattern 2 from error.h)
 *   Returns 1 = true (code object is held, safe to use)
 *   Returns 0 = false (not held, may be invalid)
 *
 * @param code_addr Raw PyCodeObject* pointer.
 * @return 1 if held by registry, 0 otherwise.
 */
int code_registry_is_held(uintptr_t code_addr);

/**
 * Get the current GC epoch (generation count).
 *
 * This is the sum of all GC generations' collection counts.
 * If this value changes between capture and resolution, GC has run.
 *
 * Thread safety: Requires GIL.
 * Async-signal safety: NO.
 *
 * @return Current GC epoch.
 */
uint64_t code_registry_get_gc_epoch(void);

/**
 * Get registry statistics.
 *
 * @param refs_held      Output: number of references currently held.
 * @param refs_added     Output: total references added.
 * @param refs_released  Output: total references released.
 * @param validations    Output: total validation calls.
 * @param invalid_count  Output: validations that returned invalid.
 */
void code_registry_get_stats(
    uint64_t* refs_held,
    uint64_t* refs_added,
    uint64_t* refs_released,
    uint64_t* validations,
    uint64_t* invalid_count
);

/**
 * Reset all registry statistics.
 */
void code_registry_reset_stats(void);

/**
 * Clear all held references.
 *
 * Use this when restarting profiling to release any stale references.
 * Requires GIL.
 */
void code_registry_clear_all(void);

/**
 * Enable or disable "safe mode" for production deployments.
 *
 * When safe mode is enabled, code_registry_validate() will reject any
 * code objects that are NOT held by the registry (i.e., not INCREF'd).
 *
 * This addresses a potential race condition in signal-handler captured
 * samples (Linux): between capture and resolution, GC could free the
 * code object and the memory could be reused. While PyCode_Check() is
 * generally safe after basic pointer validation, in edge cases with
 * aggressive memory reuse, it could still access freed memory.
 *
 * Trade-off:
 *   - Disabled (default): All samples processed, tiny theoretical risk
 *   - Enabled: Signal-handler samples on Linux may be discarded, but
 *              guaranteed memory safety
 *
 * Note: Darwin/Mach sampler holds the GIL during capture and INCREFs
 * code objects, so safe mode has no effect on Darwin samples.
 *
 * @param enabled 1 to enable safe mode, 0 to disable.
 */
void code_registry_set_safe_mode(int enabled);

/**
 * Check if safe mode is currently enabled.
 *
 * @return 1 if safe mode is enabled, 0 otherwise.
 */
int code_registry_is_safe_mode(void);

/**
 * Get extended statistics including safe mode rejections.
 *
 * @param refs_held        Output: number of references currently held.
 * @param refs_added       Output: total references added.
 * @param refs_released    Output: total references released.
 * @param validations      Output: total validation calls.
 * @param invalid_count    Output: validations that returned invalid.
 * @param safe_mode_rejects Output: samples discarded due to safe mode.
 */
void code_registry_get_stats_extended(
    uint64_t* refs_held,
    uint64_t* refs_added,
    uint64_t* refs_released,
    uint64_t* validations,
    uint64_t* invalid_count,
    uint64_t* safe_mode_rejects
);

#ifdef __cplusplus
}
#endif

#endif /* SPPROF_CODE_REGISTRY_H */

