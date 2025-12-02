# Data Model: Linux Free-Threading Support

**Feature**: 005-linux-freethreading  
**Date**: December 2, 2024

## Entities

### ValidationState (Cached at Init)

Immutable state cached during module initialization, used during speculative capture.

| Field | Type | Description |
|-------|------|-------------|
| `cached_code_type` | `PyTypeObject*` | Pointer to `&PyCode_Type`, cached at init for signal-safe comparison |
| `heap_lower_bound` | `uintptr_t` | Minimum valid heap address (0x10000 default) |
| `heap_upper_bound` | `uintptr_t` | Maximum valid heap address (47-bit x86-64 / 48-bit ARM64 limit) |
| `initialized` | `int` | Whether validation state has been initialized |

**Lifecycle**: Created once during `PyInit__native()`, never modified, never freed.

**Invariants**:
- `cached_code_type` is never NULL after initialization
- Bounds are constant for process lifetime

---

### CycleDetector (Stack-Allocated)

Per-sample detection of circular frame chains.

| Field | Type | Description |
|-------|------|-------------|
| `seen` | `uintptr_t[8]` | Rolling window of recently visited frame addresses |
| `seen_idx` | `int` | Current write position (wraps with `& 7`) |

**Lifecycle**: Created on stack at start of frame capture, discarded after sample complete.

**Invariants**:
- Size is exactly 8 (fits in cache line)
- Index wraps using bitmask, never bounds-checked

---

### SpeculativeSample (Transient)

Raw sample data captured during speculative frame walk.

| Field | Type | Description |
|-------|------|-------------|
| `frames` | `uintptr_t[128]` | Code object pointers (validated) |
| `instr_ptrs` | `uintptr_t[128]` | Instruction pointers for line resolution |
| `depth` | `int` | Number of valid frames captured |
| `validation_failed` | `int` | Whether any validation check failed |

**Lifecycle**: Stack-allocated in signal handler, copied to ring buffer if valid.

**Invariants**:
- `depth <= 128` (SPPROF_MAX_STACK_DEPTH)
- If `validation_failed == 1`, sample is dropped (not written to ring buffer)

---

### SampleStatistics (Global Atomics)

Counters for profiling session statistics.

| Field | Type | Description |
|-------|------|-------------|
| `samples_captured` | `_Atomic uint64_t` | Samples successfully written to ring buffer |
| `samples_dropped` | `_Atomic uint64_t` | Samples dropped (buffer full) |
| `samples_dropped_validation` | `_Atomic uint64_t` | Samples dropped due to validation failure |

**Lifecycle**: Reset at profiler start, accumulated during session, read at stop.

**Invariants**:
- All counters monotonically increase during session
- `samples_captured + samples_dropped + samples_dropped_validation = total_signals_received`

---

## State Transitions

### Profiler Lifecycle with Free-Threading

```text
                    ┌─────────────────────────────────────┐
                    │           NOT_STARTED               │
                    └──────────────┬──────────────────────┘
                                   │ profiler.start()
                                   ▼
                    ┌─────────────────────────────────────┐
                    │ INIT_VALIDATION_STATE               │
                    │  - Cache PyCode_Type                │
                    │  - Set heap bounds                  │
                    │  - Reset statistics                 │
                    └──────────────┬──────────────────────┘
                                   │
                                   ▼
                    ┌─────────────────────────────────────┐
                    │            SAMPLING                 │
                    │  - SIGPROF fires                    │
                    │  - Speculative capture with         │
                    │    validation                       │
                    │  - Write to ring buffer or drop     │
                    └──────────────┬──────────────────────┘
                                   │ profiler.stop()
                                   ▼
                    ┌─────────────────────────────────────┐
                    │            STOPPED                  │
                    │  - Statistics available             │
                    │  - Samples resolved                 │
                    └─────────────────────────────────────┘
```

### Sample Capture State Machine

```text
SIGPROF received
       │
       ▼
┌──────────────┐    tstate invalid     ┌─────────────────┐
│ Get TState   │ ─────────────────────►│ DROP (no count) │
└──────┬───────┘                       └─────────────────┘
       │ tstate valid
       ▼
┌──────────────┐    frame invalid      ┌─────────────────┐
│ Get Frame    │ ─────────────────────►│ DONE (write)    │
└──────┬───────┘                       └─────────────────┘
       │ frame valid
       ▼
┌──────────────┐    fails              ┌─────────────────┐
│ Validate Ptr │ ─────────────────────►│ DROP+COUNT      │
└──────┬───────┘                       └─────────────────┘
       │ passes
       ▼
┌──────────────┐    cycle found        ┌─────────────────┐
│ Cycle Check  │ ─────────────────────►│ DROP+COUNT      │
└──────┬───────┘                       └─────────────────┘
       │ no cycle
       ▼
┌──────────────┐    not code object    ┌─────────────────┐
│ Type Check   │ ─────────────────────►│ SKIP FRAME      │
└──────┬───────┘                       │ (continue walk) │
       │ is code                       └─────────────────┘
       ▼
┌──────────────┐
│ Store Frame  │
│ Get Previous │──────► (loop to Get Frame)
└──────────────┘
```

---

## Validation Rules

### Pointer Validation

```text
ptr_valid(p):
  1. p != NULL
  2. (uintptr_t)p >= HEAP_LOWER_BOUND (0x10000)
  3. (uintptr_t)p <= HEAP_UPPER_BOUND (0x7FFFFFFFFFFF for x86-64)
  4. (uintptr_t)p & 0x7 == 0  (8-byte aligned)
```

### Code Object Validation

```text
looks_like_code(obj):
  1. ptr_valid(obj)
  2. ptr_valid(obj->ob_type)
  3. obj->ob_type == cached_code_type
```

### Cycle Detection

```text
is_cycle(frame, seen[], depth):
  for i in 0..min(8, depth):
    if seen[i] == frame:
      return true
  return false
```

---

## Memory Layout

### ARM64 Memory Barrier Placement

```text
Frame Chain Walk with Barriers (ARM64):

  ┌─────────────────────────────────────────────────────────────┐
  │  frame = atomic_load_acquire(&tstate->current_frame)        │
  └─────────────────────┬───────────────────────────────────────┘
                        │
                        ▼
  ┌─────────────────────────────────────────────────────────────┐
  │  ACQUIRE BARRIER ensures we see all stores before frame ptr │
  └─────────────────────┬───────────────────────────────────────┘
                        │
                        ▼ (loop)
  ┌─────────────────────────────────────────────────────────────┐
  │  code = frame->f_executable  (plain load OK - same thread)  │
  │  prev = atomic_load_acquire(&frame->previous)               │
  └─────────────────────────────────────────────────────────────┘
```

### Tagged Pointer Layout (Python 3.14)

```text
_PyStackRef.bits (64-bit):

  ┌────────────────────────────────────────────────────────┬───┬───┐
  │                    PyObject* address                   │ R │ D │
  └────────────────────────────────────────────────────────┴───┴───┘
   63                                                      2   1   0

  D (bit 0): Deferred reference flag
  R (bit 1): Reserved

  Extraction: ptr = bits & ~0x3
```

