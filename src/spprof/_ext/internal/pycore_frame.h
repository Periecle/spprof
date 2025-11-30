/**
 * internal/pycore_frame.h - Python internal frame structures
 *
 * This header defines Python's internal frame structures for async-signal-safe
 * frame walking. These structures mirror CPython's internal layout and must be
 * kept in sync with the Python version being built against.
 *
 * CRITICAL: This code is version-specific. The struct layouts MUST match
 * what CPython uses internally, or memory access will be incorrect.
 *
 * Supported versions:
 *   - Python 3.11.x
 *   - Python 3.12.x
 *   - Python 3.13.x
 *
 * References:
 *   - CPython Include/internal/pycore_frame.h
 *   - CPython Include/cpython/pystate.h
 *
 * Copyright (c) 2024 spprof contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef SPPROF_INTERNAL_PYCORE_FRAME_H
#define SPPROF_INTERNAL_PYCORE_FRAME_H

#include <Python.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * =============================================================================
 * Version Detection
 * =============================================================================
 */

#define SPPROF_PY311 (PY_VERSION_HEX >= 0x030B0000 && PY_VERSION_HEX < 0x030C0000)
#define SPPROF_PY312 (PY_VERSION_HEX >= 0x030C0000 && PY_VERSION_HEX < 0x030D0000)
#define SPPROF_PY313 (PY_VERSION_HEX >= 0x030D0000 && PY_VERSION_HEX < 0x030E0000)
#define SPPROF_PY314 (PY_VERSION_HEX >= 0x030E0000)

#if PY_VERSION_HEX < 0x030B0000
    #error "spprof internal API requires Python 3.11 or later"
#endif

/*
 * Frame ownership constants - consistent across all versions
 */
enum _spprof_frameowner {
    SPPROF_FRAME_OWNED_BY_THREAD = 0,
    SPPROF_FRAME_OWNED_BY_GENERATOR = 1,
    SPPROF_FRAME_OWNED_BY_FRAME_OBJECT = 2,
    SPPROF_FRAME_OWNED_BY_CSTACK = 3,
};

/*
 * =============================================================================
 * Python 3.11 Frame Structures
 * =============================================================================
 *
 * In 3.11, _PyInterpreterFrame is the internal frame representation.
 * PyFrameObject wraps it for user-visible frames.
 *
 * Thread state has: cframe -> current_frame -> previous
 */
#if SPPROF_PY311

typedef struct _spprof_InterpreterFrame {
    /* First fields must match CPython exactly */
    PyCodeObject *f_code;        /* Strong ref to code object */
    struct _spprof_InterpreterFrame *previous;
    PyObject *f_funcobj;         /* Strong ref to function */
    PyObject *f_globals;         /* Borrowed ref to globals */
    PyObject *f_builtins;        /* Borrowed ref to builtins */
    PyObject *f_locals;          /* Strong ref, may be NULL */
    PyFrameObject *frame_obj;    /* Weak ref to frame object */
    _Py_CODEUNIT *prev_instr;    /* Last executed instruction */
    int stacktop;                /* Offset of TOS from localsplus */
    uint16_t yield_offset;       /* For generators */
    char owner;                  /* enum _frameowner */
    /* Followed by localsplus array */
} _spprof_InterpreterFrame;

/* 
 * CFrame links C and Python frames in 3.11
 */
typedef struct _spprof_CFrame {
    struct _spprof_CFrame *previous;
    _spprof_InterpreterFrame *current_frame;
    int use_tracing;
} _spprof_CFrame;

static inline _spprof_InterpreterFrame*
_spprof_get_current_frame(PyThreadState *tstate) {
    if (tstate == NULL) return NULL;
    /* In 3.11, tstate->cframe->current_frame */
    _spprof_CFrame *cf = (_spprof_CFrame *)tstate->cframe;
    return cf ? cf->current_frame : NULL;
}

static inline PyCodeObject*
_spprof_frame_get_code(_spprof_InterpreterFrame *frame) {
    return frame ? frame->f_code : NULL;
}

#endif /* SPPROF_PY311 */

/*
 * =============================================================================
 * Python 3.12 Frame Structures
 * =============================================================================
 *
 * Major changes in 3.12:
 *   - CFrame removed, current_frame directly in tstate
 *   - prev_instr renamed to instr_ptr
 *   - f_code renamed to f_executable in some contexts
 */
#if SPPROF_PY312

/*
 * Python 3.12 _PyInterpreterFrame - EXACT layout from CPython 3.12 source
 * From Include/internal/pycore_frame.h
 */
typedef struct _spprof_InterpreterFrame {
    PyCodeObject *f_code;        /* Strong reference - first field! */
    struct _spprof_InterpreterFrame *previous;
    PyObject *f_funcobj;
    PyObject *f_globals;
    PyObject *f_builtins;
    PyObject *f_locals;
    PyFrameObject *frame_obj;
    _Py_CODEUNIT *prev_instr;    /* NOT instr_ptr - named prev_instr in 3.12 */
    int stacktop;
    uint16_t return_offset;
    char owner;
    /* Followed by localsplus array */
} _spprof_InterpreterFrame;

/*
 * _PyCFrame structure (still exists in 3.12)
 */
typedef struct _spprof_CFrame {
    struct _spprof_InterpreterFrame *current_frame;
    struct _spprof_CFrame *previous;
} _spprof_CFrame;

static inline _spprof_InterpreterFrame*
_spprof_get_current_frame(PyThreadState *tstate) {
    if (tstate == NULL) return NULL;
    /* Python 3.12 uses cframe->current_frame */
    _spprof_CFrame *cf = (_spprof_CFrame *)tstate->cframe;
    return cf ? cf->current_frame : NULL;
}

static inline PyCodeObject*
_spprof_frame_get_code(_spprof_InterpreterFrame *frame) {
    if (frame == NULL) return NULL;
    /* In 3.12, f_code is directly a PyCodeObject* */
    return frame->f_code;
}

#endif /* SPPROF_PY312 */

/*
 * =============================================================================
 * Python 3.13+ Frame Structures
 * =============================================================================
 *
 * 3.13 changes:
 *   - f_executable replaces f_code (more general)
 *   - Free-threading support
 *   - current_frame is properly in tstate
 */
#if SPPROF_PY313 || SPPROF_PY314

typedef struct _spprof_InterpreterFrame {
    PyObject *f_executable;      /* Code object or other callable */
    struct _spprof_InterpreterFrame *previous;
    PyObject *f_funcobj;
    PyObject *f_globals;
    PyObject *f_builtins;
    PyObject *f_locals;
    PyFrameObject *frame_obj;
    _Py_CODEUNIT *instr_ptr;
    int stacktop;
    uint16_t return_offset;
    char owner;
} _spprof_InterpreterFrame;

static inline _spprof_InterpreterFrame*
_spprof_get_current_frame(PyThreadState *tstate) {
    if (tstate == NULL) return NULL;
    /* 3.13 has current_frame directly accessible */
    return (_spprof_InterpreterFrame *)tstate->current_frame;
}

static inline PyCodeObject*
_spprof_frame_get_code(_spprof_InterpreterFrame *frame) {
    if (frame == NULL) return NULL;
    PyObject *exec = frame->f_executable;
    if (exec == NULL) return NULL;
    /* f_executable could be non-code in some cases */
    if (PyCode_Check(exec)) {
        return (PyCodeObject *)exec;
    }
    return NULL;
}

#endif /* SPPROF_PY313 || SPPROF_PY314 */

/*
 * =============================================================================
 * Common Functions (all versions)
 * =============================================================================
 */

static inline _spprof_InterpreterFrame*
_spprof_frame_get_previous(_spprof_InterpreterFrame *frame) {
    return frame ? frame->previous : NULL;
}

static inline int
_spprof_frame_is_shim(_spprof_InterpreterFrame *frame) {
    return frame && frame->owner == SPPROF_FRAME_OWNED_BY_CSTACK;
}

static inline int
_spprof_frame_get_owner(_spprof_InterpreterFrame *frame) {
    return frame ? frame->owner : -1;
}

/*
 * Get instruction pointer for line number calculation
 */
static inline _Py_CODEUNIT*
_spprof_frame_get_instr_ptr(_spprof_InterpreterFrame *frame) {
    if (frame == NULL) return NULL;
#if SPPROF_PY311 || SPPROF_PY312
    return frame->prev_instr;
#else
    /* Python 3.13+ uses instr_ptr */
    return frame->instr_ptr;
#endif
}

#ifdef __cplusplus
}
#endif

#endif /* SPPROF_INTERNAL_PYCORE_FRAME_H */
