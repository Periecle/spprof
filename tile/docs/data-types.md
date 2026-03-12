# Data Types

All data types used by the spprof public API.

## Sample Data Types

### Frame

A single frame in a call stack. Used in `Sample.frames` and `AggregatedStack.frames`.

```python { .api }
from dataclasses import dataclass

@dataclass(frozen=True)
class Frame:
    function_name: str    # Function or method name
    filename: str         # Source file path
    lineno: int           # Line number in source file
    is_native: bool       # True if this is a C/native frame (default: False)
```

### Sample

A single profiling sample captured at one point in time.

```python { .api }
from dataclasses import dataclass
from typing import Sequence

@dataclass(frozen=True)
class Sample:
    timestamp_ns: int             # Capture timestamp in nanoseconds
    thread_id: int                # OS thread identifier
    thread_name: str | None       # Thread name if available
    frames: Sequence[Frame]       # Call stack, bottom-to-top order (innermost last)
```

Note: `frames` is ordered bottom-to-top — `frames[0]` is the outermost (root) frame, `frames[-1]` is the innermost (current executing) frame.

### StackTrace

A unique call stack represented as an immutable, hashable tuple. Used internally during aggregation.

```python { .api }
from dataclasses import dataclass

@dataclass(frozen=True)
class StackTrace:
    frames: tuple[Frame, ...]    # Immutable tuple of frames (hashable)
    thread_id: int
    thread_name: str | None      # Default: None

    def __hash__(self) -> int: ...  # Hashes on (frames, thread_id)
```

### AggregatedStack

A call stack with its occurrence count, as stored in `AggregatedProfile.stacks`.

```python { .api }
from dataclasses import dataclass
from typing import Sequence

@dataclass
class AggregatedStack:
    frames: Sequence[Frame]    # Call stack frames
    thread_id: int
    thread_name: str | None
    count: int                 # Number of times this exact stack was sampled
```

## Statistics Types

### ProfilerStats

Live profiling statistics, returned by `spprof.stats()` while profiling is active.

```python { .api }
from dataclasses import dataclass

@dataclass
class ProfilerStats:
    collected_samples: int        # Number of samples captured so far
    dropped_samples: int          # Samples lost due to buffer overflow
    duration_ms: float            # Time elapsed since start() in milliseconds
    overhead_estimate_pct: float  # Estimated profiler overhead as percentage
```

Overhead is estimated assuming ~25 microseconds per sample handler invocation.

## Native Frame Types

### NativeFrame

A C/C++ stack frame, returned only by `spprof.capture_native_stack()`.

```python { .api }
from dataclasses import dataclass

@dataclass(frozen=True)
class NativeFrame:
    ip: int          # Instruction pointer (memory address)
    symbol: str      # Resolved symbol/function name (empty if unresolved)
    filename: str    # Object file path (shared library or executable)
    offset: int      # Byte offset from the start of the symbol
    resolved: bool   # True if symbol name was successfully resolved
```

Note: Native frames appearing within profiling samples (from `Profile.samples`) are represented as `Frame` objects with `is_native=True`, not as `NativeFrame` objects. `NativeFrame` is only used by `capture_native_stack()`.

## Module-Level Constants

### `__version__`

```python { .api }
__version__: str  # Current package version, e.g. "0.1.0"
```

```python
import spprof
print(spprof.__version__)  # "0.1.0"
```

## Type Summary

| Type | Mutable | Hashable | Used In |
|------|---------|----------|---------|
| `Frame` | No (frozen) | Yes | `Sample.frames`, `AggregatedStack.frames` |
| `Sample` | No (frozen) | No | `Profile.samples` |
| `StackTrace` | No (frozen) | Yes | Internal aggregation |
| `AggregatedStack` | Yes | No | `AggregatedProfile.stacks` |
| `ProfilerStats` | Yes | No | `stats()` return value |
| `NativeFrame` | No (frozen) | No | `capture_native_stack()` return value |
