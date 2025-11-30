# Data Model: Python Sampling Profiler

**Feature Branch**: `001-python-sampling-profiler`  
**Date**: 2025-11-29

---

## Core Entities

### 1. RawSample (C Layer)

Captured in signal handler context. Contains only raw pointers and integers—no strings or Python objects.

| Field | Type | Description |
|-------|------|-------------|
| `timestamp` | `uint64_t` | Monotonic clock value (rdtsc or clock_gettime) |
| `thread_id` | `uint64_t` | OS thread ID (pthread_self or GetCurrentThreadId) |
| `depth` | `int` | Number of valid frame pointers |
| `frames` | `uintptr_t[128]` | Raw PyCodeObject* pointers (unresolved) |

**Constraints**:
- Fixed size: 1032 bytes (8 + 8 + 4 + 4 padding + 128×8)
- Signal-safe: No heap allocation, no pointer dereference
- Max depth: 128 frames (deeper stacks truncated)

---

### 2. ResolvedFrame (Python/C Layer)

Produced by symbol resolution in consumer thread.

| Field | Type | Description |
|-------|------|-------------|
| `function_name` | `str` | Python function name (from `co_name`) |
| `filename` | `str` | Source file path (from `co_filename`) |
| `lineno` | `int` | Line number (computed from instruction offset) |
| `is_native` | `bool` | True if this is a shim frame (C extension call) |

**Relationships**:
- Derived from `RawSample.frames[i]` via PyCodeObject introspection
- Cached by code object address for performance

---

### 3. Sample (Python Layer)

Aggregated sample ready for output formatting.

| Field | Type | Description |
|-------|------|-------------|
| `timestamp_ns` | `int` | Unix timestamp in nanoseconds |
| `thread_id` | `int` | Python thread ID (threading.get_ident compatible) |
| `thread_name` | `str | None` | Thread name if available |
| `frames` | `list[ResolvedFrame]` | Stack trace, bottom to top |

**State Transitions**: None (immutable after creation)

---

### 4. Profile (Python Layer)

Collection of samples from a profiling session.

| Field | Type | Description |
|-------|------|-------------|
| `start_time` | `datetime` | When profiling started |
| `end_time` | `datetime` | When profiling stopped |
| `interval_ms` | `int` | Configured sampling interval |
| `samples` | `list[Sample]` | All collected samples |
| `dropped_count` | `int` | Samples dropped due to ring buffer overflow |
| `python_version` | `str` | e.g., "3.12.1" |
| `platform` | `str` | e.g., "Linux-5.15-x86_64" |

**Validation Rules**:
- `interval_ms` must be ≥ 1
- `end_time` must be ≥ `start_time`
- `samples` may be empty (valid zero-sample profile)

---

### 5. RingBuffer (C Layer)

Lock-free queue for signal-to-thread communication.

| Field | Type | Description |
|-------|------|-------------|
| `write_idx` | `_Atomic uint64_t` | Next write position (producer) |
| `read_idx` | `_Atomic uint64_t` | Next read position (consumer) |
| `samples` | `RawSample[65536]` | Pre-allocated sample slots |
| `capacity` | `const size_t` | 65536 (power of 2) |

**Invariants**:
- `(write_idx - read_idx) <= capacity` (never overwrite unread)
- Single producer (signal handler), single consumer (resolver thread)
- Memory order: release on write, acquire on read

---

### 6. ProfilerState (C Layer)

Global profiler state machine.

| Field | Type | Description |
|-------|------|-------------|
| `state` | `enum {IDLE, RUNNING, STOPPING}` | Current profiler state |
| `ring_buffer` | `RingBuffer*` | Active ring buffer |
| `resolver_thread` | `pthread_t` | Consumer thread handle |
| `timer_id` | `timer_t` | Active timer (Linux) |
| `interval_ns` | `uint64_t` | Sampling interval |

**State Transitions**:

```
IDLE ──start()──► RUNNING ──stop()──► STOPPING ──cleanup──► IDLE
  ▲                                        │
  └────────────────────────────────────────┘
```

---

## Entity Relationships

```
┌─────────────────────────────────────────────────────────────────┐
│                         ProfilerState                           │
│  (singleton, manages lifecycle)                                 │
└─────────────────────────────────────────────────────────────────┘
           │ owns                            │ creates
           ▼                                 ▼
┌─────────────────────┐           ┌─────────────────────┐
│     RingBuffer      │           │   resolver_thread   │
│  (lock-free queue)  │──────────►│  (consumer)         │
└─────────────────────┘           └─────────────────────┘
           ▲                                 │
           │ writes                          │ reads & resolves
           │                                 ▼
┌─────────────────────┐           ┌─────────────────────┐
│    signal_handler   │           │   ResolvedFrame     │
│  (producer)         │           │   (cached)          │
└─────────────────────┘           └─────────────────────┘
           │                                 │
           │ captures                        │ aggregates into
           ▼                                 ▼
┌─────────────────────┐           ┌─────────────────────┐
│     RawSample       │           │       Sample        │
│  (signal-safe)      │           │  (Python object)    │
└─────────────────────┘           └─────────────────────┘
                                             │
                                             │ collected into
                                             ▼
                                  ┌─────────────────────┐
                                  │       Profile       │
                                  │  (session result)   │
                                  └─────────────────────┘
```

---

## Output Format: Speedscope JSON

The `Profile` entity serializes to Speedscope JSON format:

```json
{
  "$schema": "https://www.speedscope.app/file-format-schema.json",
  "version": "1.0.0",
  "shared": {
    "frames": [
      {"name": "main", "file": "app.py", "line": 10},
      {"name": "process", "file": "worker.py", "line": 42}
    ]
  },
  "profiles": [
    {
      "type": "sampled",
      "name": "Thread-1",
      "unit": "nanoseconds",
      "startValue": 0,
      "endValue": 1000000000,
      "samples": [[0, 1], [1, 0]],
      "weights": [10000000, 10000000]
    }
  ],
  "name": "spprof profile",
  "exporter": "spprof 1.0.0"
}
```

---

## Validation Rules Summary

| Entity | Rule | Error Handling |
|--------|------|----------------|
| RawSample | depth ≤ 128 | Truncate stack |
| RingBuffer | write_idx - read_idx < capacity | Drop sample, increment counter |
| ResolvedFrame | code object still valid | Skip frame, mark as "[unknown]" |
| Profile | interval_ms ≥ 1 | Raise ValueError at start() |
| ProfilerState | Only one RUNNING at a time | Raise RuntimeError on double start |

