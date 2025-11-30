/**
 * compat/py313.h - Python 3.13 frame structure compatibility
 *
 * Uses public Python API for portability.
 * Python 3.13 has free-threading support but we use the same
 * public API which is thread-safe.
 */

#ifndef SPPROF_COMPAT_PY313_H
#define SPPROF_COMPAT_PY313_H

#include <Python.h>
#include <frameobject.h>
#include <stdint.h>

/**
 * Get current frame from thread state using public API.
 *
 * NOTE: On Linux, this is called from signal handler context where the signal
 * fires in the thread's own context, so PyEval_GetFrame() is correct.
 * On Windows, the caller should use PyThreadState_GetFrame() directly
 * since the timer runs in a different thread.
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
    Py_DECREF(code);
    return addr;
}

/**
 * Check if frame is a C-extension shim
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

#endif /* SPPROF_COMPAT_PY313_H */
