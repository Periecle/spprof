/**
 * compat/py312.h - Python 3.12+ frame structure compatibility
 *
 * Uses public Python API for portability. While internal structures
 * would be more efficient, they require building against Python source.
 */

#ifndef SPPROF_COMPAT_PY312_H
#define SPPROF_COMPAT_PY312_H

#include <Python.h>
#include <frameobject.h>
#include <stdint.h>

/**
 * Get current frame from thread state using public API
 */
static inline void* compat_get_current_frame(PyThreadState* tstate) {
    if (tstate == NULL) {
        return NULL;
    }
    /* Use public API - returns PyFrameObject* */
    return (void*)PyEval_GetFrame();
}

/**
 * Get previous frame in chain
 */
static inline void* compat_get_previous_frame(void* frame) {
    if (frame == NULL) {
        return NULL;
    }
    PyFrameObject* f = (PyFrameObject*)frame;
    return (void*)PyFrame_GetBack(f);
}

/**
 * Get code object address
 */
static inline uintptr_t compat_get_code_addr(void* frame) {
    if (frame == NULL) {
        return 0;
    }
    PyFrameObject* f = (PyFrameObject*)frame;
    PyCodeObject* code = PyFrame_GetCode(f);
    if (code == NULL) {
        return 0;
    }
    uintptr_t addr = (uintptr_t)code;
    Py_DECREF(code);  /* PyFrame_GetCode returns new reference */
    return addr;
}

/**
 * Check if frame is a C-extension shim
 * In public API, we check if code object is NULL
 */
static inline int compat_is_shim_frame(void* frame) {
    if (frame == NULL) {
        return 0;
    }
    PyFrameObject* f = (PyFrameObject*)frame;
    PyCodeObject* code = PyFrame_GetCode(f);
    if (code == NULL) {
        return 1;
    }
    Py_DECREF(code);
    return 0;
}

#endif /* SPPROF_COMPAT_PY312_H */
