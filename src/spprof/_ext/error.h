/**
 * error.h - Common error handling conventions and types for spprof
 *
 * This header defines the error handling patterns used throughout the spprof
 * C extension modules. All modules should follow these conventions for
 * consistency and maintainability.
 *
 * =============================================================================
 * ERROR HANDLING CONVENTIONS
 * =============================================================================
 *
 * PATTERN 1: POSIX-Style (Lifecycle Functions)
 * --------------------------------------------
 * Use for: init, cleanup, start, stop, and other lifecycle operations
 * Returns: 0 on success, -1 on error
 * Error detail: Set errno before returning -1 (when applicable)
 *
 * Example:
 *     int module_init(void) {
 *         if (failed) {
 *             errno = ENOMEM;
 *             return -1;
 *         }
 *         return 0;
 *     }
 *
 * PATTERN 2: Boolean Success (Try Operations)
 * -------------------------------------------
 * Use for: Operations that may or may not succeed, queries, availability checks
 * Returns: 1 on success/true, 0 on failure/false
 * Naming: Prefer prefix like "try_", "is_", "has_", "can_" to clarify semantics
 *
 * Example:
 *     int ringbuffer_write(RingBuffer* rb, const Sample* s);  // 1=written, 0=full
 *     int cache_lookup(uintptr_t key, Value* out);            // 1=found, 0=not found
 *
 * PATTERN 3: Result Enum (Rich Error Information)
 * -----------------------------------------------
 * Use for: Operations with multiple distinct failure modes
 * Returns: SpResult or domain-specific enum
 * Naming: Use "_Result" suffix for domain-specific enums
 *
 * Example:
 *     CodeValidationResult code_registry_validate(uintptr_t addr, uint64_t epoch);
 *
 * =============================================================================
 * COMMON ERROR CODES
 * =============================================================================
 */

#ifndef SPPROF_ERROR_H
#define SPPROF_ERROR_H

#include <errno.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * SpResult - Common result type for operations that can fail in multiple ways
 *
 * Use this enum when an operation has multiple distinct failure modes that
 * the caller might want to handle differently.
 *
 * For simple success/failure, prefer returning int (0 or -1, or 1 or 0).
 */
typedef enum {
    /* Success */
    SP_OK = 0,                   /* Operation succeeded */

    /* Generic errors */
    SP_ERR_INIT = -1,            /* Not initialized or init failed */
    SP_ERR_NOMEM = -2,           /* Memory allocation failed */
    SP_ERR_INVALID = -3,         /* Invalid argument or state */
    SP_ERR_BUSY = -4,            /* Resource busy (already in use) */
    SP_ERR_NOT_FOUND = -5,       /* Resource not found */
    SP_ERR_FULL = -6,            /* Buffer or queue full */
    SP_ERR_EMPTY = -7,           /* Buffer or queue empty */
    SP_ERR_TIMEOUT = -8,         /* Operation timed out */
    SP_ERR_PERMISSION = -9,      /* Permission denied */

    /* Platform-specific errors (range -100 to -199) */
    SP_ERR_PLATFORM = -100,      /* Generic platform error */
    SP_ERR_THREAD_SUSPEND = -101,/* Thread suspension failed */
    SP_ERR_THREAD_RESUME = -102, /* Thread resume failed */
    SP_ERR_THREAD_STATE = -103,  /* Could not get thread state */
    SP_ERR_MACH_KERNEL = -104,   /* Mach kernel error (check errno) */

    /* Python-specific errors (range -200 to -299) */
    SP_ERR_PYTHON = -200,        /* Generic Python error */
    SP_ERR_NO_GIL = -201,        /* GIL not held when required */
    SP_ERR_INVALID_CODE = -202,  /* Invalid PyCodeObject */
    SP_ERR_GC_STALE = -203,      /* Object may have been GC'd */
} SpResult;

/**
 * Check if a SpResult indicates success.
 */
static inline int sp_succeeded(SpResult result) {
    return result == SP_OK;
}

/**
 * Check if a SpResult indicates failure.
 */
static inline int sp_failed(SpResult result) {
    return result != SP_OK;
}

/**
 * Convert SpResult to a human-readable error message.
 *
 * Thread safety: Safe (returns static strings).
 *
 * @param result The result code to convert.
 * @return Static string describing the error.
 */
static inline const char* sp_result_str(SpResult result) {
    switch (result) {
        case SP_OK:               return "success";
        case SP_ERR_INIT:         return "not initialized or init failed";
        case SP_ERR_NOMEM:        return "memory allocation failed";
        case SP_ERR_INVALID:      return "invalid argument or state";
        case SP_ERR_BUSY:         return "resource busy";
        case SP_ERR_NOT_FOUND:    return "resource not found";
        case SP_ERR_FULL:         return "buffer full";
        case SP_ERR_EMPTY:        return "buffer empty";
        case SP_ERR_TIMEOUT:      return "operation timed out";
        case SP_ERR_PERMISSION:   return "permission denied";
        case SP_ERR_PLATFORM:     return "platform error";
        case SP_ERR_THREAD_SUSPEND: return "thread suspension failed";
        case SP_ERR_THREAD_RESUME:  return "thread resume failed";
        case SP_ERR_THREAD_STATE:   return "could not get thread state";
        case SP_ERR_MACH_KERNEL:    return "Mach kernel error";
        case SP_ERR_PYTHON:         return "Python error";
        case SP_ERR_NO_GIL:         return "GIL not held";
        case SP_ERR_INVALID_CODE:   return "invalid code object";
        case SP_ERR_GC_STALE:       return "object may have been garbage collected";
        default:                    return "unknown error";
    }
}

/**
 * Map errno to SpResult for common cases.
 *
 * Use this when wrapping POSIX functions that set errno.
 *
 * @param err_num The errno value to convert.
 * @return Corresponding SpResult, or SP_ERR_PLATFORM for unmapped errors.
 */
static inline SpResult sp_from_errno(int err_num) {
    switch (err_num) {
        case 0:       return SP_OK;
        case ENOMEM:  return SP_ERR_NOMEM;
        case EINVAL:  return SP_ERR_INVALID;
        case EBUSY:   return SP_ERR_BUSY;
        case ENOENT:  return SP_ERR_NOT_FOUND;
        case EAGAIN:  return SP_ERR_BUSY;
        case ESRCH:   return SP_ERR_NOT_FOUND;
        case EPERM:   return SP_ERR_PERMISSION;
        case EACCES:  return SP_ERR_PERMISSION;
        default:      return SP_ERR_PLATFORM;
    }
}

/*
 * =============================================================================
 * MIGRATION MACROS
 * =============================================================================
 *
 * These macros help migrate existing code to consistent patterns while
 * maintaining backward compatibility.
 */

/**
 * SP_CHECK_INIT - Validate that a module is initialized.
 *
 * Usage:
 *     SP_CHECK_INIT(g_initialized, -1);  // Returns -1 if not initialized
 *
 * @param init_flag The initialization flag to check.
 * @param ret_val   Value to return if not initialized.
 */
#define SP_CHECK_INIT(init_flag, ret_val) \
    do { \
        if (!(init_flag)) { \
            return (ret_val); \
        } \
    } while (0)

/**
 * SP_CHECK_NULL - Validate that a pointer is not NULL.
 *
 * Usage:
 *     SP_CHECK_NULL(ptr, -1);  // Returns -1 if ptr is NULL
 *
 * @param ptr     The pointer to check.
 * @param ret_val Value to return if NULL.
 */
#define SP_CHECK_NULL(ptr, ret_val) \
    do { \
        if ((ptr) == NULL) { \
            return (ret_val); \
        } \
    } while (0)

/**
 * SP_CHECK_ERRNO - Check errno and return SpResult.
 *
 * Usage:
 *     if (some_posix_call() < 0) {
 *         SP_CHECK_ERRNO();  // Returns mapped SpResult
 *     }
 */
#define SP_CHECK_ERRNO() return sp_from_errno(errno)

#ifdef __cplusplus
}
#endif

#endif /* SPPROF_ERROR_H */


