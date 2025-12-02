# spprof Architecture

This document describes the internal architecture of spprof, a high-performance sampling profiler for Python.

## Overview

spprof is a high-performance sampling profiler for Python that captures call stacks with minimal overhead. The architecture varies by platform:

| Platform | Sampling Method | Free-Threading Safe |
|----------|-----------------|---------------------|
| **Linux** | SIGPROF signal + per-thread timers | ✅ Yes (speculative capture) |
| **macOS** | Mach thread suspension | ✅ Yes |
| **Windows** | Timer queue + GIL acquisition | ✅ Yes |

The architecture consists of:

1. **Sampler** - Platform-specific sampling (signal handler or thread suspension)
2. **Ring Buffer** - Lock-free queue for sampler→resolver communication
3. **Code Registry** - Reference tracking to prevent use-after-free
4. **Resolver** - Converts raw pointers to human-readable symbols
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
│  │   Platform  │  │   Sampler   │  │    Ring     │  │  Resolver  │ │
│  │   (Linux:   │─▶│   (Linux:   │─▶│   Buffer    │─▶│ (GIL, sym  │ │
│  │   timer,    │  │   signal,   │  │  (lock-free │  │  cache,    │ │
│  │   Darwin:   │  │   Darwin:   │  │    SPSC)    │  │  dladdr)   │ │
│  │   Mach,     │  │   suspend,  │  │             │  │            │ │
│  │   Windows:  │  │   Windows:  │  │      │      │  │            │ │
│  │   timer Q)  │  │   GIL+walk) │  │      ▼      │  │            │ │
│  └─────────────┘  └─────────────┘  │ Code Regist │  └────────────┘ │
│                                    │ (ref track) │                  │
│                                    └─────────────┘                  │
└─────────────────────────────────────────────────────────────────────┘
```

## Directory Structure

```
src/spprof/
├── __init__.py          # Public Python API
├── output.py            # Output formatters (Speedscope, FlameGraph)
├── _ext/                # C extension source code
│   ├── module.c         # Python extension entry point
│   ├── signal_handler.c # Async-signal-safe signal handler (Linux)
│   ├── ringbuffer.c     # Lock-free SPSC queue
│   ├── resolver.c       # Symbol resolution with mixed-mode merging
│   ├── framewalker.c    # Frame walking (vtable dispatch)
│   ├── unwind.c         # Native stack unwinding (libunwind/backtrace)
│   ├── code_registry.c  # Code object reference tracking
│   ├── internal/        # Python internal structure definitions
│   │   ├── pycore_frame.h   # _PyInterpreterFrame for 3.11-3.14
│   │   └── pycore_tstate.h  # Async-signal-safe frame capture
│   ├── compat/          # Public API compatibility (fallback)
│   │   └── py*.h        # Version-specific headers
│   └── platform/        # Platform-specific implementations
│       ├── linux.c      # timer_create + SIGEV_THREAD_ID + SIGPROF
│       ├── darwin.c     # Legacy setitimer wrapper (delegates to Mach)
│       ├── darwin_mach.c # Mach sampler: thread suspension + frame walking
│       └── windows.c    # Timer queue + GIL + CaptureStackBackTrace
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
- **Symbol cache**: 4-way set-associative cache with pseudo-LRU eviction
- **Line number resolution**: Uses instruction pointer for accuracy
- **Mixed-mode merging**: "Trim & Sandwich" algorithm combines native and Python frames
- **Native symbol resolution**: Uses `dladdr()` (POSIX) or `DbgHelp` (Windows)
- **Batch processing**: Drains ring buffer efficiently via streaming API

### 5. Code Registry (`code_registry.c`)

The Code Registry tracks references to `PyCodeObject` pointers captured during sampling to prevent use-after-free bugs.

**Problem Solved**:

```
Timeline without registry:
  T1: Sample captured, frame.code = 0x7fff12345678 (PyCodeObject*)
  T2: Python GC runs, frees the code object
  T3: Memory at 0x7fff12345678 is reused for something else
  T4: Resolver tries to read code->co_name → crash or garbage
```

**Solution**:

```c
// During sampling (Darwin Mach sampler, with GIL held):
python_depth = capture_frames(tstate, frames, ...);
code_registry_add_refs_batch(frames, python_depth, gc_epoch);  // Py_INCREF

// During resolution (later):
CodeValidationResult result = code_registry_validate(code_addr, epoch);
if (result == CODE_VALID) {
    // Safe to dereference
}

// After resolution:
code_registry_release_refs_batch(frames, python_depth);  // Py_DECREF
```

**Platform Differences**:

| Platform | Reference Holding | Safety Guarantee |
|----------|-------------------|------------------|
| Darwin/Mach | ✅ INCREF during capture (GIL held) | Guaranteed valid |
| Linux/SIGPROF | ❌ Cannot INCREF (signal context) | Best-effort validation |
| Windows | ✅ INCREF during capture (GIL held) | Guaranteed valid |

**Safe Mode**:

For Linux signal-handler samples (where we can't INCREF), Safe Mode discards any code objects not already tracked:

```c
code_registry_set_safe_mode(1);  // Enable safe mode

// In validation:
if (g_safe_mode && !code_registry_is_held(code_addr)) {
    g_safe_mode_rejects++;
    return CODE_INVALID_NOT_HELD;  // Discard this frame
}
```

This trades profile completeness for guaranteed memory safety.

### 6. Platform Layer (`platform/`)

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

**Container Fallback**: When `CLOCK_THREAD_CPUTIME_ID` fails (common in containers with restricted syscalls), spprof automatically falls back to `CLOCK_MONOTONIC` (wall-time). This is transparent to the user but means sleeping threads will also be sampled.

**Thread Registry (Dynamic Thread Tracking)**

The Linux implementation uses a hash table (uthash) for dynamic thread tracking:

```c
typedef struct ThreadTimerEntry {
    pid_t tid;              // Key: Linux thread ID
    timer_t timer_id;       // POSIX timer handle
    uint64_t overruns;      // Accumulated timer overruns
    int active;             // Timer running state
    UT_hash_handle hh;      // uthash handle
} ThreadTimerEntry;
```

Key features:
- **No thread limits**: Dynamic growth replaces the old 256-thread limit
- **O(1) operations**: Hash table provides constant-time lookup
- **RWLock protection**: Concurrent reads during profiling, serialized writes
- **Overrun tracking**: Per-thread overrun counts aggregated globally
- **Race-free shutdown**: Signal blocking during cleanup prevents crashes

Thread registry operations:
| Operation | Complexity | Thread Safety |
|-----------|------------|---------------|
| `registry_add_thread` | O(1) avg | Write lock |
| `registry_find_thread` | O(1) avg | Read lock |
| `registry_remove_thread` | O(1) avg | Write lock |
| `registry_count` | O(1) | Read lock |

**Timer Cleanup**

Safe timer deletion uses signal blocking:

```c
// Block SIGPROF to prevent races
sigset_t block_set;
sigemptyset(&block_set);
sigaddset(&block_set, SIGPROF);
pthread_sigmask(SIG_BLOCK, &block_set, &old_set);

// Delete timer safely
timer_delete(timer_id);

// Drain pending signals
while (sigtimedwait(&block_set, &info, &timeout) > 0) {}

// Restore signal mask
pthread_sigmask(SIG_SETMASK, &old_set, NULL);
```

**Pause/Resume Support**

Timers can be paused without destruction:

```c
// Pause: Set zero interval (disarm)
struct itimerspec zero = {0};
timer_settime(timer_id, 0, &zero, NULL);

// Resume: Restore saved interval
timer_settime(timer_id, 0, &saved_interval, NULL);
```

#### Linux Free-Threading (Speculative Capture)

Python 3.13+ can be built with free-threading support (`--disable-gil`). spprof fully supports free-threaded Python on Linux via a speculative capture mechanism with multi-layer validation.

**The Challenge:**

Signal-based sampling on free-threaded Python is inherently racy because frame chains can change while being read. Unlike macOS where we can suspend threads, Linux signal handlers must work with a potentially-changing frame chain.

**Solution: Speculative Capture with Validation**

```c
// Speculative capture with validation
_spprof_InterpreterFrame *frame = SPPROF_ATOMIC_LOAD_PTR(&tstate->current_frame);

while (frame && depth < max_frames) {
    // 1. Validate pointer bounds and alignment
    if (!_spprof_ptr_valid_speculative(frame)) break;
    
    // 2. Cycle detection (prevent infinite loops from corruption)
    if (seen_before(frame)) { drop_sample(); return 0; }
    
    // 3. Type-check code object using cached PyCode_Type pointer
    if (_spprof_looks_like_code(code)) {
        frames[depth++] = code;
    }
    
    // 4. Move to next frame with atomic load
    frame = SPPROF_ATOMIC_LOAD_PTR(&frame->previous);
}
```

**Safety Model:**

| Check | Purpose |
|-------|---------|
| Pointer bounds | Detect freed/corrupted memory (heap range validation) |
| 8-byte alignment | All Python objects are aligned |
| Cycle detection | Prevent infinite loops from corrupted frame chains |
| Type validation | Verify code object via cached `PyCode_Type` pointer |

**Performance Characteristics:**

- Race window during frame updates: ~10-50ns
- Sampling interval: 10ms (default)
- Collision probability: ~0.0005% per sample
- Corrupted samples are safely dropped (increments `validation_drops` counter)

**Memory Ordering:**

- x86-64: Strong memory model, plain loads sufficient
- ARM64: Uses acquire semantics via `SPPROF_ATOMIC_LOAD_PTR`

**Monitoring Validation Drops:**

```python
stats = spprof.stats()
# Check validation_drops in extended stats for free-threading health
```

#### macOS (`darwin_mach.c`) - Mach-Based Sampler

macOS uses a Mach-based "Suspend-Walk-Resume" sampling pattern that is fundamentally different from signal-based sampling on Linux. This design is **required** for several reasons and provides important safety guarantees.

**Core Approach**:

```c
// Sampler thread loop (runs at configurable interval)
void sample_all_threads() {
    PyGILState_STATE gstate = PyGILState_Ensure();
    
    for (each python_thread in interpreter) {
        thread_suspend(mach_port);           // Fully stop target thread
        capture_python_frames(tstate);       // Walk Python frame chain
        capture_native_frames(registers);    // Walk C stack via frame pointers
        code_registry_add_refs_batch(...);   // INCREF code objects
        thread_resume(mach_port);            // Resume target thread
        write_sample_to_ringbuffer(...);
    }
    
    PyGILState_Release(gstate);
}
```

**Key Components**:

| Component | Purpose |
|-----------|---------|
| `pthread_introspection_hook` | Automatic thread discovery (lifecycle events) |
| `thread_suspend()`/`thread_resume()` | Safely stop thread for inspection |
| `thread_get_state()` | Capture registers (PC, SP, FP) for native stack |
| `mach_wait_until()` | Precise timing without signal overhead |
| Thread Registry | Track all threads with Mach ports and stack bounds |

**Thread Discovery**:

```c
// Automatically called when threads start/terminate
void introspection_hook(unsigned int event, pthread_t thread, ...) {
    switch (event) {
        case PTHREAD_INTROSPECTION_THREAD_START:
            registry_add(&g_state.registry, thread);
            break;
        case PTHREAD_INTROSPECTION_THREAD_TERMINATE:
            registry_remove(&g_state.registry, thread);
            break;
    }
}
```

**Benefits over Signal-Based Sampling**:

1. **Free-Threading Safe**: Works with Python 3.13+ `--disable-gil` builds
2. **Deterministic Sampling**: Precise control over which threads are sampled
3. **No Signal Handler Constraints**: Can use any API during frame capture
4. **Native Stack Integration**: Frame pointer walking from captured registers

---

### Mach Sampler Design Decisions

This section documents key design decisions in the Mach sampler and explains why certain alternative approaches were rejected.

#### Why the GIL is Held During the Entire Sampling Loop

A common suggestion is to minimize GIL hold time by:
1. Using `task_threads()` to discover threads instead of Python APIs
2. Suspending threads and walking stacks WITHOUT the GIL
3. Only acquiring GIL at the end to INCREF captured code objects

**This approach is fundamentally flawed.** Here's why:

**Problem 1: `task_threads()` Returns Mach Ports, Not Python Thread States**

```c
// This does NOT work for finding Python frames:
thread_act_port_array_t threads;
task_threads(mach_task_self(), &threads, &count);

// We get Mach port numbers, but:
// - No mapping to PyThreadState
// - No access to Python frame chain
// - No way to know which threads are Python threads
```

The Python frame chain (`_PyInterpreterFrame`) is a linked list starting from `PyThreadState->current_frame`. There is no way to find this from a Mach port alone. We MUST iterate Python's thread state list.

**Problem 2: Thread State List Requires GIL**

```c
// These APIs require GIL for safe linked-list traversal:
PyThreadState* tstate = PyInterpreterState_ThreadHead(interp);
while (tstate != NULL) {
    // Process thread...
    tstate = PyThreadState_Next(tstate);  // Follows linked list
}
```

Without the GIL, another thread could modify the linked list (add/remove thread states) while we're iterating, leading to use-after-free or infinite loops.

**Problem 3: Code Object INCREFs Cannot Be Deferred**

```c
// In the current design:
python_depth = _spprof_capture_frames_from_tstate(tstate, frames, ...);
code_registry_add_refs_batch(frames, python_depth, gc_epoch);  // INCREF here
thread_resume(mach_port);
```

If we tried to defer INCREF to after all threads are resumed:
1. GC could run between resume and INCREF
2. Code objects could be freed and memory reused
3. INCREF would operate on invalid/reused memory

**The Safe Ordering**:
```
suspend → capture frames → INCREF (while suspended) → resume
```

The INCREF must happen while the thread is still suspended AND we hold the GIL, ensuring the captured pointer is valid.

**Problem 4: Caching PyThreadState Pointers is Unsafe**

```c
// This is UNSAFE:
// Step 1: Cache all thread states (with GIL)
for (tstate = ThreadHead(interp); tstate; tstate = Next(tstate)) {
    cached_tstates[i++] = tstate;
}
PyGILState_Release(gstate);  // Release GIL

// Step 2: Suspend and sample (without GIL)
for (i = 0; i < count; i++) {
    suspend_and_sample(cached_tstates[i]);  // CRASH: tstate may be freed!
}
```

A Python thread could exit between step 1 and step 2. The cached `PyThreadState*` would become a dangling pointer.

#### Why Not Use a Lock-Free Thread State Cache?

Another suggestion is to maintain our own lock-free cache of thread states, updated by Python callbacks.

**Problems**:
1. Python doesn't provide callbacks for thread state changes that work in free-threaded builds
2. The `threading` module's hooks run too late (after thread is fully initialized)
3. Would need to solve the same dangling pointer problem

The `pthread_introspection_hook` we use provides thread LIFECYCLE events (start/terminate), but gives us `pthread_t` handles, not `PyThreadState*` pointers. We still need the GIL to map these to Python thread states.

#### Performance Characteristics and Trade-offs

**Current Design Performance** (measured on Apple M1):

| Thread Count | GIL Hold Time | Per-Thread Overhead |
|--------------|---------------|---------------------|
| 1 thread | ~30μs | ~30μs |
| 10 threads | ~300μs | ~30μs each |
| 50 threads | ~1.5ms | ~30μs each |
| 100 threads | ~3ms | ~30μs each |

**Impact Analysis**:

For a 10ms sampling interval (100 Hz):
- 10 threads: 300μs / 10ms = 3% overhead
- 50 threads: 1.5ms / 10ms = 15% overhead
- 100 threads: 3ms / 10ms = 30% overhead

**Recommendations for High-Thread-Count Workloads**:
1. Increase sampling interval (e.g., 20ms instead of 10ms)
2. Profile only a subset of threads (feature TODO)
3. Use shorter profiling sessions

**Why This Overhead is Acceptable**:

1. The GIL is only held during the sampling tick, not continuously
2. Python bytecode execution pauses, but C extensions that release the GIL continue
3. Individual threads are suspended for only ~10-50μs each
4. The alternative (unsafe sampling) risks crashes and corrupted data

#### Free-Threading (Py_GIL_DISABLED) Safety

The Mach sampler is **safe** for free-threaded Python builds because:

1. **Thread Suspension**: `thread_suspend()` fully stops the target thread
2. **Stable Frame Chain**: Frame pointers cannot change while suspended
3. **Critical Section**: `PyGILState_Ensure()` acquires the appropriate lock
4. **Order of Operations**: Suspend → Read → INCREF → Resume

Linux signal-based sampling required special handling for free-threading safety. See the [Linux Free-Threading section](#linux-free-threading-speculative-capture) for details on how speculative capture with validation makes this safe.

```c
// From pycore_frame.h:
#if SPPROF_FREE_THREADED
    #if defined(__APPLE__)
        #define SPPROF_FREE_THREADING_SAFE 1  // Mach sampler is safe
    #elif defined(__linux__)
        #define SPPROF_FREE_THREADING_SAFE 1  // Speculative capture is safe
    #else
        #define SPPROF_FREE_THREADING_SAFE 0  // Other platforms not yet supported
    #endif
#endif
```

#### Symbol Resolution Safety

Native symbol resolution via `dladdr()` is **not** done during thread suspension:

```c
// WRONG (deadlock risk):
thread_suspend(port);
dladdr(pc, &info);  // dladdr may take loader lock → deadlock if target holds it
thread_resume(port);

// CORRECT (current implementation):
thread_suspend(port);
native_stack.frames[i].return_addr = pc;  // Just copy PC address
thread_resume(port);
// ... later, in resolver (after resume) ...
dladdr(pc, &info);  // Safe - target thread is running
```

If the target thread was suspended while holding the loader lock (e.g., during `dlopen`), calling `dladdr()` would deadlock. We defer symbol resolution to the resolver, which runs after all threads are resumed.

#### Summary: Why the Current Design is Correct

| Requirement | Current Design | Alternative (No GIL) |
|-------------|----------------|----------------------|
| Find Python threads | ✅ PyInterpreterState_ThreadHead (safe) | ❌ task_threads() (no PyThreadState mapping) |
| Iterate thread list | ✅ GIL protects linked list | ❌ List can change during iteration |
| Access frame chain | ✅ Thread suspended, chain stable | ❌ Would need to cache, risk dangling ptrs |
| INCREF code objects | ✅ Done while suspended + GIL | ❌ Cannot defer (GC race) |
| Free-threading safe | ✅ Works with Py_GIL_DISABLED | ✅ Linux uses speculative capture |

The GIL hold time is an acceptable trade-off for correctness and safety. The profiler is designed for observability, not to be invisible. A few milliseconds of GIL hold time per sample is preferable to crashes, corrupted profiles, or use-after-free bugs.

#### Windows (`windows.c`)

Uses timer queue with GIL acquisition (no thread suspension needed):

```c
// Timer callback (runs in thread pool thread)
void CALLBACK timer_callback(PVOID param, BOOLEAN timer_fired) {
    PyGILState_STATE gstate = PyGILState_Ensure();
    
    // With GIL held, iterate all Python threads
    PyThreadState* tstate = PyInterpreterState_ThreadHead(interp);
    while (tstate != NULL) {
        // Walk frames using public API (PyThreadState_GetFrame, etc.)
        walk_thread_frames(tstate, &sample, line_numbers);
        
        // Optionally capture native stack
        if (g_native_unwinding) {
            capture_native_stack(native_frames, MAX_NATIVE_FRAMES, 2);
        }
        
        tstate = PyThreadState_Next(tstate);
    }
    
    PyGILState_Release(gstate);
}
```

**Key Features**:
- Uses `CaptureStackBackTrace()` for native frames (fast, no DbgHelp lock)
- Accurate line numbers via `PyFrame_GetLineNumber()`
- Sample batching for reduced ring buffer contention
- No thread suspension needed (GIL provides synchronization)

**Why No Thread Suspension on Windows?**

Unlike macOS Mach, Windows thread suspension (`SuspendThread`) has several issues:
1. Can deadlock if target holds critical section
2. Requires elevated privileges in some configurations
3. GIL already provides sufficient synchronization for Python frame walking

Since we need the GIL anyway to safely iterate thread states and INCREF code objects, we simply hold it throughout the sample cycle.

### Mixed-Mode Frame Merging ("Trim & Sandwich" Algorithm)

The resolver merges native C/C++ frames with Python frames to create coherent mixed-mode stack traces:

```
Captured Native Stack:          Captured Python Stack:
[0] some_c_function             [0] my_python_func
[1] PyObject_Call               [1] call_helper
[2] _PyEval_EvalFrameDefault    [2] main
[3] PyEval_EvalCode
[4] run_mod
[5] PyRun_FileExFlags
[6] Py_RunMain
[7] main

Merged Result:
[0] some_c_function        (native - leaf)
[1] my_python_func         (python)
[2] call_helper            (python)
[3] main                   (python)
[4] Py_RunMain             (native - entry)
[5] main                   (native - entry)
```

**Algorithm**:

```c
int merge_native_and_python_frames(...) {
    // Walk native stack from leaf (index 0)
    for (i = 0; i < native_depth; i++) {
        resolve_native_frame(native_pcs[i], &frame, &is_interpreter);
        
        if (is_interpreter && !python_inserted) {
            // Hit Python interpreter - INSERT PYTHON STACK HERE
            for (j = 0; j < python_depth; j++) {
                resolve_code_object(python_frames[j], &out_frames[out_idx++]);
            }
            python_inserted = 1;
            // Skip remaining interpreter frames
        } else if (!is_interpreter) {
            // Non-interpreter native frame - include it
            out_frames[out_idx++] = frame;
        }
    }
}
```

**Interpreter Detection**:

Uses address-based comparison for robustness:

```c
// During resolver init:
Dl_info info;
dladdr((void*)&Py_Initialize, &info);
g_python_lib_base = info.dli_fbase;  // Base address of Python library

// During frame processing:
int is_python_interpreter_frame(void* lib_base, const char* lib_path) {
    // Primary: Compare library base addresses
    if (g_python_lib_base != NULL && lib_base == g_python_lib_base) {
        return 1;
    }
    // Fallback: String heuristics
    if (strstr(lib_path, "Python.framework") || strstr(lib_path, "libpython")) {
        return 1;
    }
    return 0;
}
```

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
2. **Ring buffer full**: Samples dropped, counter incremented (check `profile.dropped_count`)
3. **Invalid code objects in resolver**: Skip frame, log error
4. **Thread termination**: Timers cleaned up automatically
5. **Container restrictions**: Automatic fallback to wall-time sampling when CPU-time timers fail

For troubleshooting common issues, see the [Troubleshooting Guide](TROUBLESHOOTING.md).

## Future Improvements

### Completed

- ✅ **Sample aggregation**: Reduce memory for repeated stacks via `profile.aggregate()`
- ✅ **Native frame capture**: Mixed-mode profiling with C/C++ frames
- ✅ **Linux free-threading support**: Speculative capture with validation for Python 3.13+
- ✅ **macOS free-threading support**: Mach-based thread suspension sampling

### Planned

1. **Native frame interleaving**: Merge C and Python frames in call order
2. **Streaming output**: Write samples to disk for long profiles
3. **Per-thread buffers**: Reduce contention in multi-threaded apps

### Research / Long-Term

#### PEP 669 (sys.monitoring) Backend

PEP 669 (`sys.monitoring`) provides a callback-based profiling API that could offer deterministic tracing as an alternative to statistical sampling:

**Potential Benefits:**
- Zero-overhead when disabled (no signal delivery)
- Deterministic function entry/exit events
- Built-in safe points for free-threading

**Current Status:** The speculative capture approach provides good coverage with minimal drops. PEP 669 may be explored for use cases requiring deterministic tracing.

---

#### Linux: `perf_event_open` as Alternative to `timer_create`

For future enhancement, Linux's `perf_event_open` syscall with `PERF_SAMPLE_CALLCHAIN` could provide hardware-assisted sampling with lower overhead than timer-based approaches.

**Potential Benefits:**
- Hardware-assisted sampling (uses CPU performance counters)
- Lower overhead than timer interrupts
- More accurate CPU cycle attribution
- Can sample on specific events (cache misses, branches, etc.)

**Implementation Considerations:**
```c
// Conceptual perf_event_open approach
struct perf_event_attr attr = {
    .type = PERF_TYPE_SOFTWARE,
    .config = PERF_COUNT_SW_CPU_CLOCK,
    .sample_type = PERF_SAMPLE_CALLCHAIN | PERF_SAMPLE_TID,
    .sample_period = 100000,  // ~100Hz sampling
    .exclude_kernel = 1,
    .exclude_hv = 1,
};

int fd = perf_event_open(&attr, tid, -1, -1, 0);
// Memory-map ring buffer for samples
void* mmap_buf = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
```

**Challenges:**
- Requires elevated privileges or kernel `perf_event_paranoid` configuration
- Complex API with many edge cases
- Callchain only captures native frames (need separate Python frame walking)
- Memory-mapped ring buffer management adds complexity
- Not portable (Linux-only)

**Current Status:** Research phase. The current `timer_create` implementation provides good accuracy and works without elevated privileges. `perf_event_open` may be explored for scenarios requiring ultra-low overhead or hardware event correlation.

**References:**
- [perf_event_open(2) man page](https://man7.org/linux/man-pages/man2/perf_event_open.2.html)
- [Linux perf internals](https://www.brendangregg.com/perf.html)

