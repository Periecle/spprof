# Core Profiling API

The core profiling API provides three styles for capturing CPU profiles: functional (start/stop), context manager (Profiler), and decorator (@profile).

## Capabilities

### Functional API

Start and stop profiling imperatively. Use when you need fine-grained control over profiling boundaries.

```python { .api }
def start(
    interval_ms: int = 10,
    output_path: str | Path | None = None,
    memory_limit_mb: int = 100,
) -> None:
    """
    Start CPU profiling.

    Parameters:
    - interval_ms (int): Sampling interval in milliseconds. Default 10ms. Minimum 1ms.
      Lower values increase overhead. Guidelines:
        1ms  → ~5% overhead (benchmarks/short scripts)
        10ms → <1% overhead (development, default)
        100ms → <0.1% overhead (production monitoring)
    - output_path (str | Path | None): If provided, profile is automatically saved
      to this path when stop() is called. If None, profile is returned from stop().
    - memory_limit_mb (int): Maximum buffer size in MB. Default 100MB. Increase for
      long-running sessions with high dropped_count.

    Raises:
    - RuntimeError: If profiling is already active.
    - ValueError: If interval_ms < 1.
    - PermissionError: If output_path directory is not writable.
    """
```

```python { .api }
def stop() -> Profile:
    """
    Stop CPU profiling and return the collected profile.

    Returns:
    - Profile: Object containing all collected samples.

    Raises:
    - RuntimeError: If profiling is not currently active.
    """
```

```python { .api }
def is_active() -> bool:
    """
    Check whether profiling is currently active.

    Returns:
    - bool: True if profiler is running, False otherwise.
    """
```

```python { .api }
def stats() -> ProfilerStats | None:
    """
    Get live profiling statistics (query while profiling is active).

    Returns:
    - ProfilerStats: Current statistics snapshot if profiling is active.
    - None: If profiling is not active.
    """
```

Usage example:

```python
import spprof

spprof.start(interval_ms=10)

# Optionally check if running
assert spprof.is_active()

# Query live stats mid-profile
live = spprof.stats()
if live:
    print(f"Samples so far: {live.collected_samples}")
    print(f"Overhead estimate: {live.overhead_estimate_pct:.2f}%")

profile = spprof.stop()
print(f"Collected {profile.sample_count} samples in {profile.total_duration_ms:.1f}ms")
profile.save("profile.json")
```

### Context Manager (Profiler)

Profile a block of code using a `with` statement. Profile is available via `.profile` after the block exits.

```python { .api }
class Profiler:
    def __init__(
        self,
        interval_ms: int = 10,
        output_path: str | Path | None = None,
        memory_limit_mb: int = 100,
    ) -> None:
        """
        Parameters:
        - interval_ms (int): Sampling interval in milliseconds. Default 10ms.
        - output_path (str | Path | None): If set, saves profile to this path on exit.
        - memory_limit_mb (int): Maximum buffer size in MB. Default 100MB.
        """

    def __enter__(self) -> Profiler: ...

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc_val: BaseException | None,
        exc_tb: Any,
    ) -> None: ...

    @property
    def profile(self) -> Profile | None:
        """
        The collected Profile after context exit. None while profiling is in progress.
        """
```

Usage example:

```python
import spprof

with spprof.Profiler(interval_ms=5) as p:
    expensive_computation()

# Access profile after block
profile = p.profile
print(f"Effective rate: {profile.effective_rate_hz:.1f} Hz")
profile.save("profile.json")

# Auto-save variant
with spprof.Profiler(interval_ms=10, output_path="profile.json"):
    expensive_computation()
```

### Decorator (@profile)

Automatically profile a function on every call. The profile is saved to `output_path` if specified.

```python { .api }
def profile(
    interval_ms: int = 10,
    output_path: str | Path | None = None,
) -> Callable[[F], F]:
    """
    Decorator factory. Wraps the decorated function with Profiler.

    Parameters:
    - interval_ms (int): Sampling interval in milliseconds. Default 10ms.
    - output_path (str | Path | None): If set, saves profile to this path after
      each function call.

    Returns:
    - Decorated function with identical signature.

    Example:
        @spprof.profile(output_path="profile.json")
        def heavy_work():
            ...
    """
```

Usage example:

```python
import spprof

@spprof.profile(interval_ms=5, output_path="compute.json")
def heavy_computation(n):
    return sum(i * i for i in range(n))

# Profile is saved to compute.json after each call
heavy_computation(1_000_000)
```

Note: When `output_path` is not specified, the profile is collected but discarded. Use the context manager or functional API to capture the profile object.

## ProfilerStats Type

Returned by `stats()` while profiling is active.

```python { .api }
from dataclasses import dataclass

@dataclass
class ProfilerStats:
    collected_samples: int        # Samples captured so far
    dropped_samples: int          # Samples lost due to buffer overflow
    duration_ms: float            # Elapsed time in milliseconds
    overhead_estimate_pct: float  # Estimated profiling overhead (%)
```

## Configuration Reference

| Parameter | Default | Valid Range | Description |
|-----------|---------|-------------|-------------|
| `interval_ms` | `10` | `1–1000` | Sampling interval in milliseconds |
| `memory_limit_mb` | `100` | Any positive int | Max buffer size; increase if `dropped_count` is high |
| `output_path` | `None` | Any writable path | Auto-save profile on stop/exit |

## Error Handling

- `RuntimeError("Profiler already running")` — `start()` called when already active
- `RuntimeError("Profiler not running")` — `stop()` called when not active
- `ValueError("interval_ms must be >= 1")` — Invalid interval
- `PermissionError` — `output_path` directory not writable
