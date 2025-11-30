/**
 * framewalker.c - Version-polymorphic Python frame walking
 *
 * This implementation uses compile-time version dispatch to handle
 * different CPython internal frame structures across versions.
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "framewalker.h"
#include "unwind.h"

/* Include version-specific headers based on Python version */
#if PY_VERSION_HEX >= 0x030E0000  /* Python 3.14+ */
    #include "compat/py314.h"
    #define WALKER_VERSION "py314"
#elif PY_VERSION_HEX >= 0x030D0000  /* Python 3.13 */
    #include "compat/py313.h"
    #define WALKER_VERSION "py313"
#elif PY_VERSION_HEX >= 0x030C0000  /* Python 3.12 */
    #include "compat/py312.h"
    #define WALKER_VERSION "py312"
#elif PY_VERSION_HEX >= 0x030B0000  /* Python 3.11 */
    #include "compat/py311.h"
    #define WALKER_VERSION "py311"
#else  /* Python 3.9, 3.10 */
    #include "compat/py39.h"
    #define WALKER_VERSION "py39"
#endif

/* Global vtable - set during init */
static FrameWalkerVTable g_vtable;
static int g_initialized = 0;
static char g_version_info[128];
static int g_native_unwinding_enabled = 0;

int framewalker_init(void) {
    if (g_initialized) {
        return 0;
    }

    /* Set up version-specific function pointers */
    g_vtable.get_current_frame = compat_get_current_frame;
    g_vtable.get_previous_frame = compat_get_previous_frame;
    g_vtable.get_code_addr = compat_get_code_addr;
    g_vtable.is_shim_frame = compat_is_shim_frame;

    /* Build version info string */
    snprintf(g_version_info, sizeof(g_version_info),
             "%s (Python %d.%d.%d)",
             WALKER_VERSION,
             PY_MAJOR_VERSION, PY_MINOR_VERSION, PY_MICRO_VERSION);

    g_initialized = 1;
    return 0;
}

int framewalker_capture(RawFrameInfo* frames, int max_depth) {
    if (!g_initialized || max_depth <= 0) {
        return 0;
    }

    /* Get current thread state */
    PyThreadState* tstate = PyThreadState_GET();
    if (tstate == NULL) {
        return 0;
    }

    /* Get current frame */
    void* frame = g_vtable.get_current_frame(tstate);
    int depth = 0;

    /* Walk the frame chain */
    while (frame != NULL && depth < max_depth) {
        /* Skip shim frames (C extension entry points) */
        if (!g_vtable.is_shim_frame(frame)) {
            frames[depth].code_addr = g_vtable.get_code_addr(frame);
            frames[depth].is_shim = 0;
            depth++;
        }

        frame = g_vtable.get_previous_frame(frame);
    }

    return depth;
}

int framewalker_capture_raw(uintptr_t* frame_ptrs, int max_depth) {
    if (!g_initialized || max_depth <= 0) {
        return 0;
    }

    PyThreadState* tstate = PyThreadState_GET();
    if (tstate == NULL) {
        return 0;
    }

    void* frame = g_vtable.get_current_frame(tstate);
    int depth = 0;

    while (frame != NULL && depth < max_depth) {
        if (!g_vtable.is_shim_frame(frame)) {
            frame_ptrs[depth] = g_vtable.get_code_addr(frame);
            depth++;
        }
        frame = g_vtable.get_previous_frame(frame);
    }

    return depth;
}

const char* framewalker_version_info(void) {
    return g_version_info;
}

const FrameWalkerVTable* framewalker_get_vtable(void) {
    return &g_vtable;
}

int framewalker_set_native_unwinding(int enabled) {
    if (enabled && !unwind_available()) {
        return -1;  /* Not available on this platform */
    }

    if (enabled && !g_native_unwinding_enabled) {
        /* Initialize unwinding subsystem */
        if (unwind_init() < 0) {
            return -1;
        }
    }

    g_native_unwinding_enabled = enabled;
    return 0;
}

int framewalker_native_unwinding_enabled(void) {
    return g_native_unwinding_enabled;
}

int framewalker_native_unwinding_available(void) {
    return unwind_available();
}

