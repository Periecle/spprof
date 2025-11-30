# C Internal API Contract: spprof._native

**Feature Branch**: `001-python-sampling-profiler`  
**Date**: 2025-11-29

---

## Overview

The C extension (`spprof._native`) is an implementation detail. All public access is through the Python `spprof` module. This document defines the internal C API contracts for implementation reference.

---

## Module Functions (exposed to Python)

```c
// module.c - Python module definition

static PyMethodDef SpProfMethods[] = {
    {"_start", (PyCFunction)spprof_start, METH_VARARGS | METH_KEYWORDS,
     "Start profiling (internal). Use spprof.start() instead."},
    {"_stop", (PyCFunction)spprof_stop, METH_NOARGS,
     "Stop profiling and return raw samples (internal)."},
    {"_is_active", (PyCFunction)spprof_is_active, METH_NOARGS,
     "Check if profiling is active."},
    {"_get_stats", (PyCFunction)spprof_get_stats, METH_NOARGS,
     "Get current profiling statistics."},
    {"_register_thread", (PyCFunction)spprof_register_thread, METH_VARARGS,
     "Register a thread for per-thread sampling (Linux)."},
    {NULL, NULL, 0, NULL}
};
```

---

## Ring Buffer API

```c
// ringbuffer.h

#define RING_SIZE 65536
#define MAX_STACK_DEPTH 128

typedef struct {
    uint64_t timestamp;
    uint64_t thread_id;
    int depth;
    uintptr_t frames[MAX_STACK_DEPTH];
} RawSample;

typedef struct {
    _Atomic uint64_t write_idx;
    _Atomic uint64_t read_idx;
    _Atomic uint64_t dropped_count;
    RawSample samples[RING_SIZE];
} RingBuffer;

// Allocate ring buffer (called once at module init)
RingBuffer* ringbuffer_create(void);

// Free ring buffer (called at module cleanup)
void ringbuffer_destroy(RingBuffer* rb);

// Write sample (signal handler context - async-signal-safe)
// Returns: 1 on success, 0 if buffer full (sample dropped)
int ringbuffer_write(RingBuffer* rb, const RawSample* sample);

// Read sample (consumer thread context)
// Returns: 1 if sample read, 0 if buffer empty
int ringbuffer_read(RingBuffer* rb, RawSample* out);

// Check if buffer has data
int ringbuffer_has_data(RingBuffer* rb);

// Get drop count
uint64_t ringbuffer_dropped_count(RingBuffer* rb);
```

### Contract Guarantees

| Function | Thread Safety | Async-Signal Safety |
|----------|---------------|---------------------|
| `ringbuffer_create` | Not thread-safe | No |
| `ringbuffer_destroy` | Not thread-safe | No |
| `ringbuffer_write` | Single-producer safe | **Yes** |
| `ringbuffer_read` | Single-consumer safe | No |
| `ringbuffer_has_data` | Thread-safe | **Yes** |
| `ringbuffer_dropped_count` | Thread-safe | **Yes** |

---

## Frame Walker API

```c
// framewalker.h

typedef struct {
    uintptr_t code_addr;  // Raw PyCodeObject* pointer
    int is_shim;          // 1 if FRAME_OWNED_BY_CSTACK
} RawFrameInfo;

// Version-specific function table
typedef struct {
    // Get current frame from thread state
    void* (*get_current_frame)(PyThreadState* tstate);
    
    // Get previous frame in chain
    void* (*get_previous_frame)(void* frame);
    
    // Extract code object (handles tagging)
    uintptr_t (*get_code_addr)(void* frame);
    
    // Check if frame is a C-extension shim
    int (*is_shim_frame)(void* frame);
} FrameWalkerVTable;

// Initialize walker for current Python version (called at module init)
// Returns: 0 on success, -1 if version unsupported
int framewalker_init(void);

// Walk stack and fill frames array (signal handler context)
// Returns: number of frames captured
int framewalker_capture(RawFrameInfo* frames, int max_depth);

// Get Python version info
const char* framewalker_version_info(void);
```

### Version Dispatch

```c
// Compile-time version selection
#if PY_VERSION_HEX >= 0x030E0000  // 3.14+
    #include "compat/py314.h"
    #define FRAME_WALKER_IMPL py314_framewalker
#elif PY_VERSION_HEX >= 0x030D0000  // 3.13
    #include "compat/py313.h"
    #define FRAME_WALKER_IMPL py313_framewalker
#elif PY_VERSION_HEX >= 0x030C0000  // 3.12
    #include "compat/py312.h"
    #define FRAME_WALKER_IMPL py312_framewalker
#elif PY_VERSION_HEX >= 0x030B0000  // 3.11
    #include "compat/py311.h"
    #define FRAME_WALKER_IMPL py311_framewalker
#else  // 3.9, 3.10
    #include "compat/py39.h"
    #define FRAME_WALKER_IMPL py39_framewalker
#endif
```

---

## Sampler API

```c
// sampler.h

typedef enum {
    PROFILER_IDLE,
    PROFILER_RUNNING,
    PROFILER_STOPPING
} ProfilerState;

// Initialize sampler subsystem
int sampler_init(void);

// Start profiling with given interval
// Returns: 0 on success, -1 on error
int sampler_start(uint64_t interval_ns);

// Stop profiling
// Returns: 0 on success, -1 on error
int sampler_stop(void);

// Get current state
ProfilerState sampler_state(void);

// Signal handler (registered with sigaction)
void sampler_signal_handler(int sig, siginfo_t* info, void* ctx);
```

### Signal Handler Contract

The signal handler MUST:
1. Complete in < 10μs
2. NOT call `malloc()`, `free()`, or any allocator
3. NOT acquire any locks (including Python GIL)
4. NOT call most libc functions (see `man 7 signal-safety`)
5. ONLY write to pre-allocated ring buffer
6. ONLY read from pre-validated memory locations

```c
// Allowed in signal handler
void sampler_signal_handler(int sig, siginfo_t* info, void* ctx) {
    // ✓ Read thread state pointer (pre-validated)
    PyThreadState* tstate = _PyThreadState_GET();
    if (!tstate) return;
    
    // ✓ Capture timestamp (rdtsc or vDSO clock_gettime)
    uint64_t timestamp = get_monotonic_ns();
    
    // ✓ Walk frames, storing only pointers
    RawSample sample;
    sample.timestamp = timestamp;
    sample.thread_id = get_thread_id();
    sample.depth = framewalker_capture(sample.frames, MAX_STACK_DEPTH);
    
    // ✓ Write to pre-allocated ring buffer slot
    ringbuffer_write(g_ringbuffer, &sample);
}
```

---

## Resolver API

```c
// resolver.h

typedef struct {
    char function_name[256];
    char filename[1024];
    int lineno;
    int is_native;
} ResolvedFrame;

typedef struct {
    ResolvedFrame frames[MAX_STACK_DEPTH];
    int depth;
    uint64_t timestamp;
    uint64_t thread_id;
} ResolvedSample;

// Initialize resolver (starts consumer thread)
int resolver_init(RingBuffer* rb);

// Shutdown resolver (stops consumer thread)
void resolver_shutdown(void);

// Get resolved samples (called after profiling stops)
// Returns: number of samples, fills output array
int resolver_get_samples(ResolvedSample** out, size_t* count);

// Free resolved samples
void resolver_free_samples(ResolvedSample* samples, size_t count);
```

### Consumer Thread Contract

The resolver thread:
1. MAY acquire the Python GIL (briefly)
2. MAY allocate memory
3. MUST handle GC'd code objects gracefully
4. MUST cache resolved symbols for performance

---

## Platform API

```c
// platform/platform.h

// Timer setup (platform-specific implementation)
int platform_timer_create(uint64_t interval_ns);
int platform_timer_destroy(void);

// Thread ID (platform-specific)
uint64_t platform_thread_id(void);

// Monotonic timestamp (nanoseconds)
uint64_t platform_monotonic_ns(void);

// Platform name
const char* platform_name(void);
```

### Platform Implementations

| Platform | Header | Implementation |
|----------|--------|----------------|
| Linux | `platform/linux.h` | `timer_create` + SIGPROF |
| macOS | `platform/darwin.h` | `setitimer` + SIGPROF |
| Windows | `platform/windows.h` | `CreateTimerQueueTimer` + suspend |

---

## Memory Layout

```
+------------------+
|  ProfilerState   |  <- Global singleton
+------------------+
         |
         v
+------------------+
|   RingBuffer     |  <- Pre-allocated at init (64MB)
|   [65536 slots]  |
+------------------+
         |
         v (consumer thread reads)
+------------------+
|  SymbolCache     |  <- Dynamic hash table (bounded 32MB)
|   [code→info]    |
+------------------+
         |
         v
+------------------+
| ResolvedSamples  |  <- Allocated after stop()
+------------------+
```

---

## Error Codes

```c
// error.h

#define SPPROF_OK              0
#define SPPROF_ERR_ALREADY_RUNNING  -1
#define SPPROF_ERR_NOT_RUNNING      -2
#define SPPROF_ERR_TIMER_FAILED     -3
#define SPPROF_ERR_SIGNAL_FAILED    -4
#define SPPROF_ERR_THREAD_FAILED    -5
#define SPPROF_ERR_VERSION_UNSUPPORTED -6

const char* spprof_strerror(int code);
```

