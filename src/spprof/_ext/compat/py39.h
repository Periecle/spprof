/**
 * compat/py39.h - Python 3.9-3.10 frame structure compatibility
 *
 * Uses public Python API for portability.
 */

#ifndef SPPROF_COMPAT_PY39_H
#define SPPROF_COMPAT_PY39_H

#include <Python.h>
#include <frameobject.h>
#include <stdint.h>

/**
 * Get current frame from thread state
 */
static inline void* compat_get_current_frame(PyThreadState* tstate) {
    if (tstate == NULL) {
        return NULL;
    }
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
#if PY_VERSION_HEX >= 0x030B0000
    return (void*)PyFrame_GetBack(f);
#else
    return (void*)f->f_back;
#endif
}

/**
 * Get code object address
 */
static inline uintptr_t compat_get_code_addr(void* frame) {
    if (frame == NULL) {
        return 0;
    }
    PyFrameObject* f = (PyFrameObject*)frame;
#if PY_VERSION_HEX >= 0x030B0000
    PyCodeObject* code = PyFrame_GetCode(f);
    if (code == NULL) {
        return 0;
    }
    uintptr_t addr = (uintptr_t)code;
    Py_DECREF(code);
    return addr;
#else
    return (uintptr_t)f->f_code;
#endif
}

/**
 * Check if frame is a C-extension shim
 */
static inline int compat_is_shim_frame(void* frame) {
    if (frame == NULL) {
        return 0;
    }
    PyFrameObject* f = (PyFrameObject*)frame;
#if PY_VERSION_HEX >= 0x030B0000
    PyCodeObject* code = PyFrame_GetCode(f);
    if (code == NULL) {
        return 1;
    }
    Py_DECREF(code);
    return 0;
#else
    return f->f_code == NULL;
#endif
}

#endif /* SPPROF_COMPAT_PY39_H */
