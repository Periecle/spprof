# Python API Contract: Memory Profiler

**Feature**: 006-memory-profiler  
**Module**: `spprof.memprof`  
**Date**: December 3, 2024

---

## Overview

This document defines the public Python API for the memory allocation profiler. The API is designed to mirror the existing CPU profiler (`spprof`) for consistency.

---

## Core Functions

### `start(sampling_rate_kb: int = 512) -> None`

Start memory profiling.

**Parameters**:
- `sampling_rate_kb`: Average kilobytes between samples. Lower = more accuracy, higher overhead. Default 512 KB gives <0.1% overhead.

**Raises**:
- `RuntimeError`: If memory profiler is already running.
- `RuntimeError`: If interposition hooks could not be installed.
- `ValueError`: If `sampling_rate_kb < 1`.

**Example**:
```python
import spprof.memprof as memprof
memprof.start(sampling_rate_kb=256)  # More accurate
```

---

### `stop() -> None`

Stop memory profiling.

**Behavior**:
- Stops tracking NEW allocations (malloc sampling disabled)
- CONTINUES tracking frees (free lookup remains active)
- This prevents "fake leaks" where objects allocated during profiling but freed after stop() would incorrectly appear as live

**Raises**:
- `RuntimeError`: If memory profiler is not running.

**Note**: To fully disable all hooks, call `shutdown()` instead.

---

### `get_snapshot() -> HeapSnapshot`

Get snapshot of currently live (unfreed) sampled allocations.

**Returns**: `HeapSnapshot` containing all live sampled allocations.

**Thread Safety**: Can be called from any thread while profiling is active.

**Example**:
```python
snapshot = memprof.get_snapshot()
print(f"Estimated heap: {snapshot.estimated_heap_bytes / 1e9:.2f} GB")
```

---

### `get_stats() -> MemProfStats`

Get profiler statistics.

**Returns**: `MemProfStats` with current profiler state.

**Example**:
```python
stats = memprof.get_stats()
print(f"Total samples: {stats.total_samples}")
print(f"Heap map load: {stats.heap_map_load_percent:.1f}%")
```

---

### `shutdown() -> None`

Shutdown profiler and prepare for process exit.

**⚠️ WARNING**: This is a ONE-WAY operation.

**Behavior**:
- Disables all hooks (no more sampling or free tracking)
- Does NOT free internal memory (intentional, prevents crashes)
- Should only be called at process exit or before unloading the module

**Note**: After `shutdown()`, calling `start()` again raises `RuntimeError`.

---

## Data Classes

### `AllocationSample`

```python
@dataclass
class AllocationSample:
    address: int              # Pointer address
    size: int                 # Actual allocation size (bytes)
    weight: int               # Sampling weight
    estimated_bytes: int      # Contribution to heap estimate
    timestamp_ns: int         # When allocated (monotonic)
    lifetime_ns: Optional[int] # Duration if freed, None if live
    stack: List[StackFrame]   # Call stack at allocation
```

---

### `StackFrame`

```python
@dataclass
class StackFrame:
    address: int              # Raw program counter
    function: str             # Resolved function name
    file: str                 # Source file path
    line: int                 # Line number
    is_python: bool           # True if Python frame
```

---

### `HeapSnapshot`

```python
@dataclass
class HeapSnapshot:
    samples: List[AllocationSample]
    total_samples: int
    live_samples: int
    estimated_heap_bytes: int
    timestamp_ns: int
    frame_pointer_health: FramePointerHealth
```

**Methods**:

#### `top_allocators(n: int = 10) -> List[Dict]`

Get top N allocation sites by estimated bytes.

**Returns**: List of dicts with keys: `function`, `file`, `line`, `estimated_bytes`, `sample_count`.

#### `save(path: Path, format: str = "speedscope") -> None`

Save snapshot to file.

**Parameters**:
- `path`: Output file path
- `format`: `"speedscope"` (default) or `"collapsed"`

---

### `FramePointerHealth`

```python
@dataclass
class FramePointerHealth:
    shallow_stack_warnings: int
    total_native_stacks: int
    avg_native_depth: float
    min_native_depth: int
    truncation_rate: float
```

**Properties**:

#### `confidence -> str`

Returns `'high'` (<5% truncation), `'medium'` (5-20%), or `'low'` (>20%).

#### `recommendation -> Optional[str]`

Action recommendation if confidence is not high.

---

### `MemProfStats`

```python
@dataclass
class MemProfStats:
    total_samples: int
    live_samples: int
    freed_samples: int
    unique_stacks: int
    estimated_heap_bytes: int
    heap_map_load_percent: float
    collisions: int
    sampling_rate_bytes: int
```

---

## Context Manager

### `MemoryProfiler`

```python
class MemoryProfiler:
    def __init__(self, sampling_rate_kb: int = 512): ...
    def __enter__(self) -> MemoryProfiler: ...
    def __exit__(self, *args) -> None: ...
    
    @property
    def snapshot(self) -> Optional[HeapSnapshot]: ...
```

**Example**:
```python
with memprof.MemoryProfiler(sampling_rate_kb=512) as mp:
    # ... run workload ...
mp.snapshot.save("memory_profile.json")
```

---

## Usage Examples

### Basic Usage

```python
import spprof.memprof as memprof

memprof.start(sampling_rate_kb=512)

# ... application code ...
import numpy as np
data = np.random.randn(10000, 10000)

snapshot = memprof.get_snapshot()
print(f"Estimated heap: {snapshot.estimated_heap_bytes / 1e9:.2f} GB")
print(f"Live samples: {snapshot.live_samples}")

for site in snapshot.top_allocators(5):
    print(f"{site['function']}: {site['estimated_bytes'] / 1e6:.1f} MB")

memprof.stop()
```

### Combined CPU + Memory Profiling

```python
import spprof
import spprof.memprof as memprof

# Both profilers can run simultaneously
spprof.start(interval_ms=10)
memprof.start(sampling_rate_kb=512)

# ... workload ...

cpu_profile = spprof.stop()
mem_snapshot = memprof.get_snapshot()
memprof.stop()

cpu_profile.save("cpu_profile.json")
mem_snapshot.save("mem_profile.json")
```

### Low Sample Warning

```python
snapshot = memprof.get_snapshot()
if snapshot.live_samples < 100:
    print(f"⚠️ Low sample count ({snapshot.live_samples}). "
          f"Estimates may have high variance.")
```

---

## Thread Safety

| Operation | Thread Safety |
|-----------|---------------|
| `start()` | Call once from main thread |
| `stop()` | Call from any thread |
| `get_snapshot()` | Thread-safe, can be called concurrently |
| `get_stats()` | Thread-safe |
| `shutdown()` | Call once from main thread at exit |

---

## Lifecycle States

```
UNINITIALIZED ──[init()]──► INITIALIZED ──[start()]──► ACTIVE
                                                          │
                                                    [stop()]
                                                          │
                                                          ▼
                            [shutdown()]──────────────► STOPPED
                                                          │
                                                          ▼
                                                     TERMINATED
```

| State | Allowed Operations |
|-------|-------------------|
| UNINITIALIZED | `init()` (internal) |
| INITIALIZED | `start()`, `shutdown()` |
| ACTIVE | `stop()`, `get_snapshot()`, `get_stats()` |
| STOPPED | `start()`, `get_snapshot()`, `shutdown()` |
| TERMINATED | None (RuntimeError on `start()`) |

---

## Error Handling

| Error | Cause | Resolution |
|-------|-------|------------|
| `RuntimeError("Profiler already running")` | `start()` called twice | Call `stop()` first |
| `RuntimeError("Profiler not running")` | `stop()` without `start()` | Call `start()` first |
| `RuntimeError("Cannot restart after shutdown")` | `start()` after `shutdown()` | Don't call `shutdown()` until process exit |
| `RuntimeError("Interposition hooks failed")` | Platform hook installation failed | Check platform compatibility |
| `ValueError("sampling_rate_kb must be >= 1")` | Invalid parameter | Use valid sampling rate |

