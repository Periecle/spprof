# spprof Architecture

This document describes the internal architecture of spprof, a high-performance sampling profiler for Python.

## Overview

spprof uses signal-based sampling to capture Python call stacks with minimal overhead. The architecture consists of:

1. **Signal Handler** - Captures raw stack data in async-signal-safe context
2. **Ring Buffer** - Lock-free queue for signal→resolver communication
3. **Resolver** - Converts raw pointers to human-readable symbols
4. **Platform Layer** - OS-specific timer and signal handling
5. **Python API** - User-facing interface

```
┌─────────────────────────────────────────────────────────────────────┐
│                         Python Application                           │
├─────────────────────────────────────────────────────────────────────┤
│                          spprof Python API                           │
│                    start() / stop() / profile()                      │
├─────────────────────────────────────────────────────────────────────┤
│                        Native Extension Layer                        │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐  ┌────────────┐ │
│  │   Platform  │  │   Signal    │  │    Ring     │  │  Resolver  │ │
│  │   (Linux/   │─▶│   Handler   │─▶│   Buffer    │─▶│   (GIL)    │ │
│  │   Darwin/   │  │   (async-   │  │  (lock-free │  │            │ │
│  │   Windows)  │  │   safe)     │  │    SPSC)    │  │            │ │
│  └─────────────┘  └─────────────┘  └─────────────┘  └────────────┘ │
└─────────────────────────────────────────────────────────────────────┘
```

## Directory Structure

```
src/spprof/
├── __init__.py          # Public Python API
├── output.py            # Output formatters (Speedscope, FlameGraph)
├── _ext/                # C extension source code
│   ├── module.c         # Python extension entry point
│   ├── signal_handler.c # Async-signal-safe signal handler
│   ├── ringbuffer.c     # Lock-free SPSC queue
│   ├── resolver.c       # Symbol resolution
│   ├── framewalker.c    # Public API frame walker (fallback)
│   ├── framewalker_internal.c  # Internal API frame walker
│   ├── unwind.c         # Native stack unwinding
│   ├── internal/        # Python internal structure definitions
│   │   ├── pycore_frame.h   # _PyInterpreterFrame for 3.11-3.14
│   │   └── pycore_tstate.h  # Async-signal-safe frame capture
│   ├── compat/          # Public API compatibility (fallback)
│   │   └── py*.h        # Version-specific headers
│   └── platform/        # Platform-specific implementations
│       ├── linux.c      # timer_create + SIGEV_THREAD_ID
│       ├── darwin.c     # setitimer(ITIMER_PROF)
│       └── windows.c    # Timer queue + thread suspension
└── py.typed             # PEP 561 marker
```

## Components

### 1. Signal Handler (`signal_handler.c`)

The signal handler is the most critical component. It runs in async-signal-safe context, which means:

- **No malloc/free** - All storage is pre-allocated
- **No locks** - Uses atomic operations only
- **No Python API** - Reads internal structures directly
- **No I/O** - No printf, no file operations

```c
void spprof_signal_handler(int signum, siginfo_t* info, void* ucontext) {
    // 1. Check if profiler is active
    // 2. Get timestamp (async-signal-safe)
    // 3. Capture Python frames (direct memory access)
    // 4. Write to ring buffer (lock-free)
}
```

The handler captures:
- Timestamp (nanoseconds, monotonic clock)
- Thread ID
- Stack of `PyCodeObject*` pointers
- Instruction pointers (for line number resolution)

### 2. Ring Buffer (`ringbuffer.c`)

A lock-free Single-Producer Single-Consumer (SPSC) queue:

```
┌─────────────────────────────────────────────────────────────────┐
│  write_idx ──────────────▶                                      │
│  [Sample][Sample][Sample][Sample][ empty ][ empty ][ empty ]    │
│                          ▲── read_idx                           │
└─────────────────────────────────────────────────────────────────┘
```

- **Producer**: Signal handler (writes samples)
- **Consumer**: Resolver thread (reads and resolves)
- **Overflow handling**: Drops samples rather than blocking
- **Memory ordering**: Uses acquire/release semantics

The buffer is pre-allocated to avoid any allocation in the signal handler.

### 3. Frame Walker (`internal/pycore_frame.h`, `internal/pycore_tstate.h`)

Async-signal-safe frame walking using Python's internal structures:

```c
// Get thread state from TLS (async-signal-safe)
PyThreadState* tstate = _spprof_tstate_get();

// Get current frame (version-specific struct access)
_spprof_InterpreterFrame* frame = _spprof_get_current_frame(tstate);

// Walk the frame chain
while (frame != NULL) {
    PyCodeObject* code = _spprof_frame_get_code(frame);
    // Store code pointer for later resolution
    frame = _spprof_frame_get_previous(frame);
}
```

**Version Compatibility**:

| Python | Frame Access | Notes |
|--------|-------------|-------|
| 3.11 | `cframe->current_frame` | Uses `_PyCFrame` |
| 3.12 | `cframe->current_frame` | Similar to 3.11 |
| 3.13+ | `tstate->current_frame` | Direct access, `f_executable` |

### 4. Resolver (`resolver.c`)

Runs in normal context (can use GIL, malloc, Python API):

```c
int resolver_resolve_frame(uintptr_t code_addr, ResolvedFrame* out) {
    PyGILState_STATE gstate = PyGILState_Ensure();
    
    PyCodeObject* co = (PyCodeObject*)code_addr;
    // Extract: function name, filename, line number
    
    PyGILState_Release(gstate);
}
```

Features:
- **Symbol cache**: LRU cache to avoid repeated resolution
- **Line number resolution**: Uses instruction pointer for accuracy
- **Batch processing**: Drains ring buffer efficiently

### 5. Platform Layer (`platform/`)

#### Linux (`linux.c`)

Uses POSIX per-thread timers for accurate CPU-time sampling:

```c
// Create per-thread timer
struct sigevent sev = {
    .sigev_notify = SIGEV_THREAD_ID,
    .sigev_signo = SIGPROF,
    ._sigev_un._tid = gettid(),
};
timer_create(CLOCK_THREAD_CPUTIME_ID, &sev, &timer);
```

Benefits:
- Samples only when thread is executing (not sleeping)
- Each thread gets its own timer
- True per-thread CPU profiling

#### macOS (`darwin.c`)

Uses `setitimer(ITIMER_PROF)`:

```c
struct itimerval it = {
    .it_interval = { .tv_sec = 0, .tv_usec = interval_us },
    .it_value = { .tv_sec = 0, .tv_usec = interval_us },
};
setitimer(ITIMER_PROF, &it, NULL);
```

Limitations:
- Process-wide timer (not per-thread)
- Signal delivered to arbitrary thread

#### Windows (`windows.c`)

Uses timer queue with thread suspension:

```c
// Timer callback
void CALLBACK timer_callback(PVOID param, BOOLEAN timer_fired) {
    // Acquire GIL
    // Enumerate Python threads
    // For each thread:
    //   SuspendThread()
    //   Capture stack from thread state
    //   ResumeThread()
}
```

Note: Different approach due to lack of POSIX signals.

## Data Flow

```
1. Timer fires (OS)
       ↓
2. SIGPROF delivered to thread
       ↓
3. Signal handler executes:
   - Read PyThreadState from TLS
   - Walk _PyInterpreterFrame chain
   - Copy code pointers to RawSample
   - Write to ring buffer
       ↓
4. Resolver drains buffer (after profiling stops):
   - Read RawSample from ring buffer
   - Resolve code pointers to symbols
   - Build ResolvedSample with names/files/lines
       ↓
5. Python layer creates Profile object
       ↓
6. User exports to Speedscope/FlameGraph
```

## Memory Layout

### RawSample (captured in signal handler)

```
┌─────────────────────────────────────────────────────┐
│ timestamp (8 bytes)                                 │
├─────────────────────────────────────────────────────┤
│ thread_id (8 bytes)                                 │
├─────────────────────────────────────────────────────┤
│ depth (4 bytes) │ padding (4 bytes)                 │
├─────────────────────────────────────────────────────┤
│ frames[0..127] - PyCodeObject* pointers (1024 B)   │
├─────────────────────────────────────────────────────┤
│ instr_ptrs[0..127] - instruction pointers (1024 B) │
└─────────────────────────────────────────────────────┘
Total: ~2KB per sample
```

### ResolvedSample (after resolution)

```
┌─────────────────────────────────────────────────────┐
│ frames[0..127] - ResolvedFrame array                │
│   ├─ function_name (256 bytes)                      │
│   ├─ filename (1024 bytes)                          │
│   ├─ lineno (4 bytes)                               │
│   └─ is_native (4 bytes)                            │
├─────────────────────────────────────────────────────┤
│ depth (4 bytes)                                     │
├─────────────────────────────────────────────────────┤
│ timestamp (8 bytes)                                 │
├─────────────────────────────────────────────────────┤
│ thread_id (8 bytes)                                 │
└─────────────────────────────────────────────────────┘
```

## Thread Safety

| Component | Thread Safety | Notes |
|-----------|---------------|-------|
| Signal handler | Async-signal-safe | Single producer |
| Ring buffer write | Single producer | Lock-free |
| Ring buffer read | Single consumer | Lock-free |
| Resolver | GIL-protected | Safe Python API access |
| Platform timers | Thread-safe | OS-managed |

## Performance Characteristics

| Operation | Time | Notes |
|-----------|------|-------|
| Signal handler | <10μs | No allocations, direct reads |
| Frame walk (per frame) | ~100ns | Pointer chasing |
| Ring buffer write | ~50ns | Memory copy |
| Symbol resolution | ~1-10μs | Cached |
| Symbol resolution (miss) | ~10-100μs | GIL + Python API |

## Error Handling

The profiler is designed to fail safely:

1. **Invalid pointers in signal handler**: Pointer validation before dereference
2. **Ring buffer full**: Samples dropped, counter incremented
3. **Invalid code objects in resolver**: Skip frame, log error
4. **Thread termination**: Timers cleaned up automatically

## Future Improvements

1. **Native frame interleaving**: Merge C and Python frames
2. **Streaming output**: Write samples to disk for long profiles
3. **Sample aggregation**: Reduce memory for repeated stacks
4. **Per-thread buffers**: Reduce contention in multi-threaded apps

