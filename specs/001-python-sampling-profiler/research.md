# Research: Python Sampling Profiler

**Feature Branch**: `001-python-sampling-profiler`  
**Date**: 2025-11-29  
**Purpose**: Resolve technical unknowns and document architectural decisions

---

## 1. Async-Signal Safety Strategy

### Decision: Pre-allocated Ring Buffer with Deferred Symbol Resolution

The signal handler MUST NOT:
- Call `malloc()`, `free()`, or any memory allocator
- Acquire locks (including `PyGILState_Ensure()`)
- Call most libc functions (see `man signal-safety`)
- Access Python objects via normal reference counting

### Strategy

**In Signal Handler (async-signal-safe operations only):**
1. Read raw pointers from the interpreter frame chain
2. Copy pointer values (not dereference strings) into a pre-allocated ring buffer slot
3. Use atomic operations to advance the write index
4. Exit immediately

**In Background Thread (safe context):**
1. Consume ring buffer entries
2. Resolve `PyCodeObject*` pointers to function names, filenames, line numbers
3. Aggregate and format output

### Rationale
- Ring buffer uses fixed-size pre-allocated memory (no runtime allocation)
- Signal handler only writes raw pointers (8 bytes each) and integers
- Symbol resolution happens outside signal context where Python API is safe
- Pattern proven in async-profiler (JVM) and perf-map-agent

### Alternatives Considered
| Alternative | Rejected Because |
|-------------|------------------|
| Direct string copy in handler | String access requires following pointers that may be being modified; not async-signal-safe |
| Acquiring GIL in handler | Deadlock if signal arrives while GIL is held |
| Pre-resolving symbols | Would require walking all code objects upfront; misses dynamically loaded code |

---

## 2. Version Polymorphism: CPython 3.9–3.14

### Decision: Compile-Time Version Detection with Runtime Frame Walker Dispatch

Use `PY_VERSION_HEX` preprocessor macros to compile version-specific code paths, with a unified interface.

### Version-Specific Challenges

#### Python 3.9–3.10: `PyFrameObject` Era

```c
// Frame access pattern
PyFrameObject* frame = PyEval_GetFrame();
PyCodeObject* code = frame->f_code;
int lineno = PyFrame_GetLineNumber(frame);
PyFrameObject* prev = frame->f_back;
```

- Frames are heap-allocated `PyFrameObject` structures
- Direct field access to `f_code`, `f_back`, `f_lineno`
- Reference counting applies to frame objects

#### Python 3.11+: `_PyInterpreterFrame` Shift

```c
// Frame access pattern (internal API)
_PyInterpreterFrame* iframe = tstate->cframe->current_frame;  // 3.11
_PyInterpreterFrame* iframe = tstate->current_frame;          // 3.12+
PyCodeObject* code = iframe->f_executable;                    // May need untagging in 3.12+
_PyInterpreterFrame* prev = iframe->previous;
```

- Frames are stack-allocated `_PyInterpreterFrame` structures
- `PyFrameObject` still exists but is a "shadow" created on demand
- Must check `frame->owner` for shim frames (FRAME_OWNED_BY_CSTACK)

#### Python 3.12+: Tagged Pointers

```c
// f_executable may have low bits set for tagging
#define UNTAG_EXECUTABLE(ptr) ((PyCodeObject*)((uintptr_t)(ptr) & ~0x3))
PyCodeObject* code = UNTAG_EXECUTABLE(iframe->f_executable);
```

- `f_executable` uses low 2 bits for type tagging
- Must mask before dereferencing: `ptr & ~0x3`

#### Python 3.13: Free-Threaded Build

```c
// Check for free-threaded build
#ifdef Py_GIL_DISABLED
    // No GIL protection—must use atomic reads for frame pointers
    _PyInterpreterFrame* iframe = _Py_atomic_load_ptr(&tstate->current_frame);
#endif
```

- GIL-disabled builds allow true concurrent frame access
- Use atomic operations to read frame chain pointers
- Frame may be modified mid-walk by another thread

#### Python 3.14: Tail-Call Interpreter

```c
// Frame linkage preserved despite tail-call dispatch
// Key: Still walk _PyInterpreterFrame->previous chain
// Risk: C stack may be flattened; avoid libunwind for hybrid stacks
```

- Tail-call dispatch optimizes C stack but preserves Python virtual frame list
- `_PyInterpreterFrame` linked list remains walkable
- **Critical**: Do NOT rely on C stack unwinding for Python frame discovery

### Implementation Structure

```c
typedef struct {
    PyCodeObject* code;      // Untagged code object pointer
    int lineno;              // Line number (computed after handler)
    int is_shim;             // True if FRAME_OWNED_BY_CSTACK
} RawFrame;

// Version-dispatch function pointer table
typedef struct {
    _PyInterpreterFrame* (*get_current_frame)(PyThreadState*);
    _PyInterpreterFrame* (*get_previous_frame)(_PyInterpreterFrame*);
    PyCodeObject* (*get_code)(_PyInterpreterFrame*);
    int (*is_shim_frame)(_PyInterpreterFrame*);
} FrameWalkerVTable;
```

### Rationale
- Compile-time branching avoids runtime version checks in hot path
- VTable pattern allows testing each version's walker independently
- Shim frame detection prevents dereferencing invalid code objects

### Alternatives Considered
| Alternative | Rejected Because |
|-------------|------------------|
| Single generic walker | Frame structure differs too much; would require extensive runtime checks |
| Only support 3.11+ | Excludes significant user base on 3.9/3.10 |
| Use stable ABI only | `_PyInterpreterFrame` is internal; stable ABI cannot access it |

---

## 3. Lock-Free Data Ingestion: Ring Buffer Design

### Decision: Single-Producer Multi-Consumer (SPMC) Ring Buffer

The signal handler is the single producer; the background thread is the single consumer.

### Structure

```c
#define RING_SIZE 65536  // Power of 2 for fast modulo
#define MAX_STACK_DEPTH 128

typedef struct {
    uint64_t timestamp;           // rdtsc or clock_gettime result
    uint64_t thread_id;           // pthread_self() result
    int depth;                    // Number of valid frames
    uintptr_t frames[MAX_STACK_DEPTH];  // Raw PyCodeObject* pointers
} RawSample;

typedef struct {
    _Atomic uint64_t write_idx;   // Only signal handler writes
    _Atomic uint64_t read_idx;    // Only consumer thread reads
    RawSample samples[RING_SIZE];
} RingBuffer;
```

### Signal Handler Write Path

```c
void signal_handler(int sig, siginfo_t* info, void* ctx) {
    RingBuffer* rb = get_global_ring();
    
    // Acquire slot (relaxed—single producer)
    uint64_t idx = atomic_load_explicit(&rb->write_idx, memory_order_relaxed);
    uint64_t next = (idx + 1) & (RING_SIZE - 1);
    
    // Check for overflow (drop sample if full)
    if (next == atomic_load_explicit(&rb->read_idx, memory_order_acquire)) {
        atomic_fetch_add(&dropped_samples, 1);
        return;
    }
    
    RawSample* slot = &rb->samples[idx & (RING_SIZE - 1)];
    
    // Fill sample (all pointer copies, no dereferences)
    slot->timestamp = get_timestamp();
    slot->thread_id = get_thread_id();
    slot->depth = walk_frames_raw(slot->frames, MAX_STACK_DEPTH);
    
    // Publish
    atomic_store_explicit(&rb->write_idx, next, memory_order_release);
}
```

### Rationale
- Fixed-size buffer eliminates allocation
- Power-of-2 size enables bitwise modulo
- Atomic operations are async-signal-safe
- Overflow drops samples rather than blocking

### Alternatives Considered
| Alternative | Rejected Because |
|-------------|------------------|
| Linked list queue | Requires `malloc()` per sample—not async-signal-safe |
| Multiple ring buffers per thread | Complex; signal delivered to arbitrary thread |
| File-based logging | `write()` is technically safe but slow and may block |

---

## 4. Symbol Resolution Strategy

### Decision: Lazy Resolution in Consumer Thread with Code Object Cache

Symbol resolution (code → name/file/line) happens entirely in the consumer thread, not the signal handler.

### Process

1. **Signal handler**: Stores raw `PyCodeObject*` pointer in ring buffer
2. **Consumer thread**: 
   - Acquires GIL (brief, outside hot path)
   - Checks if `PyCodeObject*` still valid (may have been garbage collected)
   - Extracts `co_filename`, `co_name`, `co_firstlineno`
   - Computes exact line number from `co_linetable` (3.10+) or `co_lnotab`
   - Caches resolved frame info keyed by code object address

### Validity Check

```c
// Check if code object is still alive
int is_code_object_valid(uintptr_t ptr) {
    PyObject* obj = (PyObject*)ptr;
    // Check if it looks like a code object (weak heuristic)
    if (!PyCode_Check(obj)) return 0;
    // Additional: compare to known code objects from sys._current_frames()
    return 1;
}
```

### Cache Structure

```c
typedef struct {
    uintptr_t code_addr;          // Key: PyCodeObject* address
    char* function_name;          // Resolved co_name
    char* filename;               // Resolved co_filename
    int first_lineno;             // co_firstlineno
    uint64_t last_seen;           // For cache eviction
} ResolvedFrame;

// Hash table: code_addr → ResolvedFrame
```

### Line Number Computation

```c
int compute_lineno(PyCodeObject* code, int instruction_offset) {
#if PY_VERSION_HEX >= 0x030A0000  // 3.10+
    return PyCode_Addr2Line(code, instruction_offset * 2);
#else
    // Parse co_lnotab for 3.9
    return parse_lnotab(code->co_lnotab, instruction_offset);
#endif
}
```

### Rationale
- Deferred resolution keeps signal handler minimal
- Caching avoids repeated string extraction for hot functions
- GIL acquisition in consumer is safe and brief
- Invalid pointer detection handles GC'd code objects

### Alternatives Considered
| Alternative | Rejected Because |
|-------------|------------------|
| Pre-populate symbol table | Misses dynamically loaded modules; high startup cost |
| Store strings in signal handler | String access not async-signal-safe |
| Use instruction pointer only | Cannot get function names without code object |

---

## 5. Timer Mechanism for Multi-Thread Sampling

### Decision: `timer_create()` with `SIGEV_THREAD_ID` (Linux), `setitimer()` fallback

### Linux Strategy (Preferred)

```c
#ifdef __linux__
timer_t timerid;
struct sigevent sev = {
    .sigev_notify = SIGEV_THREAD_ID,
    .sigev_signo = SIGPROF,
    .sigev_notify_thread_id = gettid()  // Per-thread delivery
};
timer_create(CLOCK_THREAD_CPUTIME_ID, &sev, &timerid);

struct itimerspec its = {
    .it_interval = { .tv_nsec = interval_ns },
    .it_value = { .tv_nsec = interval_ns }
};
timer_settime(timerid, 0, &its, NULL);
#endif
```

### POSIX Fallback (macOS, other)

```c
struct itimerval it = {
    .it_interval = { .tv_usec = interval_us },
    .it_value = { .tv_usec = interval_us }
};
setitimer(ITIMER_PROF, &it, NULL);
```

### Windows Strategy

```c
// CreateTimerQueueTimer with callback that samples all threads
// Or: Use CPU performance counters via ETW (more complex)
HANDLE timer;
CreateTimerQueueTimer(&timer, NULL, timer_callback, 
    NULL, interval_ms, interval_ms, WT_EXECUTEINTIMERTHREAD);
```

### Thread Registration

```python
# Python side: register each thread for sampling
import threading

def _register_thread():
    """Called by each thread to set up its timer."""
    _spprof_native.register_thread(threading.get_ident())

# Patch threading.Thread.start to auto-register
```

### Rationale
- `timer_create` with `SIGEV_THREAD_ID` ensures each thread gets sampled
- `CLOCK_THREAD_CPUTIME_ID` measures CPU time, not wall time (for CPU profiling)
- Fallback to `setitimer` for portability
- Windows requires different approach (no POSIX signals)

### Alternatives Considered
| Alternative | Rejected Because |
|-------------|------------------|
| `setitimer(ITIMER_PROF)` only | Delivers to single thread; may miss workers |
| `ITIMER_REAL` | Measures wall time; includes idle time |
| pthread signal delivery | More complex; same result as `timer_create` |

---

## 6. C Extension Directory Structure

### Decision: Modular C Source with Version-Specific Compilation

```
spprof/
├── pyproject.toml              # Build config (uv, hatchling)
├── src/
│   └── spprof/
│       ├── __init__.py         # Public Python API
│       ├── _profiler.pyi       # Type stubs for C extension
│       ├── output.py           # Output formatters (Speedscope, collapsed)
│       └── _native/
│           ├── module.c        # Python module definition, init
│           ├── sampler.c       # Signal handler, timer setup
│           ├── ringbuffer.c    # Lock-free ring buffer
│           ├── framewalker.c   # Version-polymorphic frame walking
│           ├── resolver.c      # Symbol resolution (consumer side)
│           ├── platform/
│           │   ├── linux.c     # Linux-specific (timer_create)
│           │   ├── darwin.c    # macOS-specific (setitimer, mach)
│           │   └── windows.c   # Windows-specific (CreateTimerQueueTimer)
│           └── compat/
│               ├── py39.h      # Python 3.9 frame structures
│               ├── py310.h     # Python 3.10 frame structures
│               ├── py311.h     # Python 3.11 _PyInterpreterFrame
│               ├── py312.h     # Python 3.12 tagged pointers
│               ├── py313.h     # Python 3.13 free-threading
│               └── py314.h     # Python 3.14 tail-call compat
├── tests/
│   ├── test_profiler.py        # Integration tests
│   ├── test_output.py          # Output format tests
│   └── test_signal_safety.py   # Deadlock detection tests
└── benchmarks/
    └── overhead.py             # CPU overhead measurement
```

### Rationale
- `compat/` headers isolate version-specific structures
- `platform/` isolates OS-specific timer code
- Separation enables unit testing of components
- Python output module allows format changes without recompilation

---

## 7. Non-Functional Requirements (NFRs)

### Performance NFRs

| ID | Requirement | Target | Measurement |
|----|-------------|--------|-------------|
| NFR-001 | CPU overhead at 10ms sampling | < 1% | Benchmark with/without profiler |
| NFR-002 | CPU overhead at 1ms sampling | < 5% | Benchmark with/without profiler |
| NFR-003 | Signal handler execution time | < 10μs | rdtsc measurement |
| NFR-004 | Memory footprint (ring buffer) | < 16MB | Fixed: 65536 × 256 bytes |
| NFR-005 | Memory footprint (symbol cache) | < 32MB | Bounded cache with LRU eviction |
| NFR-006 | Start/stop latency | < 100ms | API timing measurement |

### Reliability NFRs

| ID | Requirement | Target | Measurement |
|----|-------------|--------|-------------|
| NFR-007 | No deadlocks in signal handler | 0 occurrences | Stress test with concurrent GIL operations |
| NFR-008 | No crashes from invalid frame pointers | 0 occurrences | Fuzz testing with GC stress |
| NFR-009 | Graceful handling of ring buffer overflow | Sample drop, not crash | Stress test with slow consumer |
| NFR-010 | Process survives profiler failure | 100% | Fault injection testing |

### Compatibility NFRs

| ID | Requirement | Target | Measurement |
|----|-------------|--------|-------------|
| NFR-011 | Python version support | 3.9–3.14 | CI matrix testing |
| NFR-012 | Platform support | Linux, macOS, Windows | CI matrix testing |
| NFR-013 | Free-threaded Python support | Full functionality | CI with `--disable-gil` build |
| NFR-014 | Container compatibility | Restricted securityContext | K8s integration test |

---

## 8. Testing Strategy for Signal Safety

### Decision: Multi-Pronged Approach

#### 8.1 Deadlock Detection Test

```python
# test_signal_safety.py
import threading
import time
import spprof

def test_no_deadlock_during_gil_operations():
    """Verify profiler doesn't deadlock when signal arrives during GIL ops."""
    
    deadlock_detected = threading.Event()
    
    def watchdog():
        time.sleep(5)
        if not deadlock_detected.is_set():
            # Test passed
            return
        # If we get here, main thread is stuck
        import os
        os._exit(1)  # Force exit on deadlock
    
    threading.Thread(target=watchdog, daemon=True).start()
    
    spprof.start(interval_ms=1)  # Aggressive sampling
    
    # Perform GIL-heavy operations that could deadlock
    for _ in range(100000):
        # Import causes GIL acquisition
        import sys
        sys.path  # Access sys module
        
        # String operations hold GIL
        s = "x" * 1000
        _ = s.upper()
    
    spprof.stop()
    deadlock_detected.set()  # Signal success
```

#### 8.2 Memory Safety Test (CI with AddressSanitizer)

```yaml
# .github/workflows/ci.yml
jobs:
  asan:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Build with ASan
        run: |
          export CFLAGS="-fsanitize=address -fno-omit-frame-pointer"
          export LDFLAGS="-fsanitize=address"
          pip install -e .
      - name: Run tests
        run: pytest tests/ -v
```

#### 8.3 Stress Test for Ring Buffer Overflow

```python
def test_ring_buffer_overflow_handling():
    """Verify samples are dropped, not crashed, on overflow."""
    spprof.start(interval_ms=1)
    
    # Burn CPU without releasing to consumer
    def busy_loop():
        x = 0
        for i in range(10_000_000):
            x += i
        return x
    
    # Run many threads to overflow buffer
    threads = [threading.Thread(target=busy_loop) for _ in range(16)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    
    stats = spprof.stop()
    
    # Should complete without crash
    # May have dropped samples
    assert stats['dropped_samples'] >= 0
    assert stats['collected_samples'] > 0
```

#### 8.4 GC Stress Test for Invalid Pointers

```python
def test_gc_stress():
    """Verify profiler handles code objects being GC'd during profiling."""
    import gc
    
    spprof.start(interval_ms=1)
    
    for _ in range(1000):
        # Create and immediately discard functions
        exec("def temp_func(): pass")
        gc.collect()
    
    # Should not crash on resolution of GC'd code objects
    profile = spprof.stop()
    assert profile is not None
```

### Rationale
- Watchdog thread detects deadlocks with timeout
- AddressSanitizer catches memory errors in CI
- Stress tests verify overflow and GC edge cases
- Tests run in CI matrix across all Python versions

---

## 9. Windows Compatibility Approach

### Decision: Suspend-and-Sample Model (No SIGPROF)

Windows lacks POSIX signals. Alternative approach:

```c
// Timer callback runs in timer thread
void CALLBACK timer_callback(PVOID param, BOOLEAN timer_fired) {
    // Enumerate Python threads
    PyGILState_STATE gstate = PyGILState_Ensure();
    PyThreadState* tstate = PyInterpreterState_ThreadHead(PyInterpreterState_Main());
    
    while (tstate != NULL) {
        // Suspend thread, read frame pointer, resume
        HANDLE thread = get_thread_handle(tstate);
        SuspendThread(thread);
        
        // Read current frame from tstate
        _PyInterpreterFrame* frame = tstate->current_frame;
        capture_stack(frame);
        
        ResumeThread(thread);
        tstate = PyThreadState_Next(tstate);
    }
    
    PyGILState_Release(gstate);
}
```

### Caveats
- Requires GIL acquisition in timer callback (safe—not in signal context)
- Thread suspension has higher overhead than signals
- May miss samples if thread is in kernel mode

### Rationale
- Only viable approach on Windows without custom driver
- Suspension is brief (~100μs per thread)
- Matches approach used by py-spy, yappi

---

## Summary of Key Decisions

| Area | Decision | Key Constraint |
|------|----------|----------------|
| Signal Safety | Pre-allocated ring buffer, deferred resolution | No malloc/locks in handler |
| Version Compat | Compile-time dispatch with VTable | Frame structures differ per version |
| Data Ingestion | SPSC ring buffer with atomics | Async-signal-safe |
| Symbol Resolution | Lazy resolution in consumer thread | GIL acquisition safe there |
| Timers (Linux) | `timer_create` with `SIGEV_THREAD_ID` | Per-thread CPU sampling |
| Timers (Windows) | Suspend-and-sample | No POSIX signals |
| Testing | Watchdog + ASan + stress tests | Prove no deadlocks |

