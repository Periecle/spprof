# spprof

A high-performance sampling profiler for Python applications. Uses signal-based sampling with a native C extension backend (pure Python fallback). Achieves less than 1% CPU overhead at the default 10ms sampling interval. Supports Linux (timer_create + SIGPROF), macOS (Mach thread suspension), and Windows (timer queue). Compatible with Python 3.9–3.14, including free-threaded builds.

## Package Information

- **Package Name**: spprof
- **Package Type**: PyPI
- **Language**: Python
- **Installation**: `pip install spprof`

## Core Imports

```python
import spprof
```

## Basic Usage

```python
import spprof

# Functional API
spprof.start(interval_ms=10)
# ... run workload ...
profile = spprof.stop()
profile.save("profile.json")  # Speedscope format, open at speedscope.app

# Context manager
with spprof.Profiler(interval_ms=5) as p:
    expensive_computation()
p.profile.save("profile.json")

# Decorator
@spprof.profile(output_path="compute.json")
def heavy_work():
    ...
```

## Capabilities

### Core Profiling API

Start/stop profiling, check status, and retrieve live statistics. Three usage styles: functional (`start`/`stop`), context manager (`Profiler`), and decorator (`@profile`).

```python { .api }
def start(interval_ms: int = 10, output_path: Path | str | None = None, memory_limit_mb: int = 100) -> None: ...
def stop() -> Profile: ...
def is_active() -> bool: ...
def stats() -> ProfilerStats | None: ...

class Profiler:
    def __init__(self, interval_ms: int = 10, output_path: Path | str | None = None, memory_limit_mb: int = 100) -> None: ...
    profile: Profile | None  # Available after context exit

def profile(interval_ms: int = 10, output_path: Path | str | None = None) -> Callable[[F], F]: ...  # Decorator factory
```

[Core Profiling API](./core-profiling.md)

### Thread Management

On Linux, each thread that should be profiled must register its own timer. On macOS and Windows, all threads are sampled automatically. Use `register_thread`/`unregister_thread` directly or the `ThreadProfiler` context manager.

```python { .api }
def register_thread() -> bool: ...
def unregister_thread() -> bool: ...

class ThreadProfiler:
    def __enter__(self) -> ThreadProfiler: ...
    def __exit__(self, exc_type, exc_val, exc_tb) -> None: ...
```

[Thread Management](./thread-management.md)

### Native Unwinding (Mixed-Mode Profiling)

Capture C/C++ frames alongside Python frames for debugging C extensions. Requires native C extension. Available on Linux (libunwind) and macOS (backtrace).

```python { .api }
def native_unwinding_available() -> bool: ...
def set_native_unwinding(enabled: bool) -> None: ...
def native_unwinding_enabled() -> bool: ...
def capture_native_stack() -> list[NativeFrame]: ...
```

[Native Unwinding](./native-unwinding.md)

### Profile Data and Output

The `Profile` and `AggregatedProfile` types hold profiling results. Both support export to Speedscope JSON and collapsed stacks (FlameGraph). Aggregate to reduce memory for long-running profiles.

```python { .api }
class Profile:
    sample_count: int          # property
    total_duration_ms: float   # property
    effective_rate_hz: float   # property
    def aggregate(self) -> AggregatedProfile: ...
    def to_speedscope(self) -> dict[str, Any]: ...
    def to_collapsed(self) -> str: ...
    def save(self, path: Path | str, format: Literal["speedscope", "collapsed"] = "speedscope") -> None: ...

class AggregatedProfile:
    unique_stack_count: int      # property
    compression_ratio: float     # property
    memory_reduction_pct: float  # property
    def to_speedscope(self) -> dict[str, Any]: ...
    def to_collapsed(self) -> str: ...
    def save(self, path: Path | str, format: Literal["speedscope", "collapsed"] = "speedscope") -> None: ...
```

[Profile Data and Output](./profile-data.md)

### Data Types

All data structures used in the API: `Frame`, `Sample`, `StackTrace`, `AggregatedStack`, `ProfilerStats`, `NativeFrame`, and the `__version__` constant.

[Data Types](./data-types.md)
