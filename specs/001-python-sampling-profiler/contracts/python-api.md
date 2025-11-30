# Python API Contract: spprof

**Feature Branch**: `001-python-sampling-profiler`  
**Date**: 2025-11-29

---

## Public API Surface

### Module: `spprof`

```python
"""High-performance sampling profiler for Python applications."""

from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Literal, Sequence

# --- Data Classes ---

@dataclass(frozen=True)
class Frame:
    """A single frame in a call stack."""
    function_name: str
    filename: str
    lineno: int
    is_native: bool = False

@dataclass(frozen=True)
class Sample:
    """A single profiling sample."""
    timestamp_ns: int
    thread_id: int
    thread_name: str | None
    frames: Sequence[Frame]  # Bottom to top

@dataclass
class Profile:
    """Result of a profiling session."""
    start_time: datetime
    end_time: datetime
    interval_ms: int
    samples: list[Sample]
    dropped_count: int
    python_version: str
    platform: str
    
    def to_speedscope(self) -> dict:
        """Convert to Speedscope JSON format."""
        ...
    
    def to_collapsed(self) -> str:
        """Convert to collapsed stack format (for FlameGraph)."""
        ...
    
    def save(self, path: Path | str, format: Literal["speedscope", "collapsed"] = "speedscope") -> None:
        """Save profile to file."""
        ...

@dataclass
class ProfilerStats:
    """Statistics from a profiling session."""
    collected_samples: int
    dropped_samples: int
    duration_ms: float
    overhead_estimate_pct: float

# --- Core API ---

def start(
    interval_ms: int = 10,
    output_path: Path | str | None = None,
) -> None:
    """
    Start CPU profiling.
    
    Args:
        interval_ms: Sampling interval in milliseconds. Default 10ms.
                    Minimum 1ms. Lower values = higher overhead.
        output_path: Optional path to write profile on stop().
                    If None, profile returned from stop().
    
    Raises:
        RuntimeError: If profiling is already active.
        ValueError: If interval_ms < 1.
        PermissionError: If output_path is not writable.
    
    Example:
        >>> import spprof
        >>> spprof.start(interval_ms=10)
        >>> # ... run workload ...
        >>> profile = spprof.stop()
    """
    ...

def stop() -> Profile:
    """
    Stop CPU profiling and return results.
    
    Returns:
        Profile object containing all collected samples.
    
    Raises:
        RuntimeError: If profiling is not active.
    
    Example:
        >>> profile = spprof.stop()
        >>> print(f"Collected {len(profile.samples)} samples")
        >>> profile.save("profile.json")
    """
    ...

def is_active() -> bool:
    """
    Check if profiling is currently active.
    
    Returns:
        True if profiler is running, False otherwise.
    """
    ...

def stats() -> ProfilerStats | None:
    """
    Get current profiling statistics.
    
    Returns:
        ProfilerStats if profiling is active, None otherwise.
    """
    ...

# --- Context Manager API ---

class Profiler:
    """
    Context manager for profiling a code block.
    
    Example:
        >>> with spprof.Profiler(interval_ms=5) as p:
        ...     # ... run workload ...
        >>> p.profile.save("profile.json")
    """
    
    def __init__(
        self,
        interval_ms: int = 10,
        output_path: Path | str | None = None,
    ) -> None:
        ...
    
    def __enter__(self) -> "Profiler":
        ...
    
    def __exit__(self, exc_type, exc_val, exc_tb) -> None:
        ...
    
    @property
    def profile(self) -> Profile | None:
        """Profile result after exiting context. None while profiling."""
        ...

# --- Decorator API ---

def profile(
    interval_ms: int = 10,
    output_path: Path | str | None = None,
):
    """
    Decorator to profile a function.
    
    Example:
        >>> @spprof.profile(output_path="compute.json")
        ... def heavy_computation():
        ...     # ... expensive work ...
    """
    ...
```

---

## Error Handling Contract

| Error | Condition | Recovery |
|-------|-----------|----------|
| `RuntimeError("Profiler already running")` | `start()` called while active | Call `stop()` first or use `is_active()` to check |
| `RuntimeError("Profiler not running")` | `stop()` called while inactive | Check `is_active()` before stopping |
| `ValueError("interval_ms must be >= 1")` | `start(interval_ms=0)` | Use valid interval |
| `PermissionError` | Output path not writable | Use writable path (e.g., /tmp) |
| `OSError` | Signal/timer setup fails | Platform-specific; check logs |

---

## Thread Safety Contract

| Operation | Thread Safety |
|-----------|---------------|
| `start()` | Must be called from main thread |
| `stop()` | May be called from any thread |
| `is_active()` | Thread-safe (atomic read) |
| `stats()` | Thread-safe (atomic reads) |
| Signal handler | Safe: no GIL, no malloc |
| Symbol resolution | Thread-safe (holds GIL briefly) |

---

## Platform Compatibility Contract

| Platform | Signal Mechanism | Timer Mechanism | Notes |
|----------|------------------|-----------------|-------|
| Linux | SIGPROF | `timer_create` | Full per-thread sampling |
| macOS | SIGPROF | `setitimer` | Process-wide signal delivery |
| Windows | N/A (suspend) | `CreateTimerQueueTimer` | Thread suspension model |

---

## Output Format Contract

### Speedscope JSON

Conforms to: https://www.speedscope.app/file-format-schema.json

```json
{
  "$schema": "https://www.speedscope.app/file-format-schema.json",
  "version": "1.0.0",
  "shared": {
    "frames": [
      {"name": "function_name", "file": "path/to/file.py", "line": 42}
    ]
  },
  "profiles": [
    {
      "type": "sampled",
      "name": "Thread-MainThread",
      "unit": "nanoseconds",
      "startValue": 0,
      "endValue": 1000000000,
      "samples": [[0, 1, 2]],
      "weights": [10000000]
    }
  ],
  "name": "spprof profile",
  "exporter": "spprof 1.0.0"
}
```

### Collapsed Stack Format

Compatible with Brendan Gregg's FlameGraph:

```
main;process_data;compute 42
main;process_data;fetch_data 18
main;handle_request;parse_json 7
```

Format: `frame1;frame2;...;frameN count`

