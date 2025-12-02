# spprof Usage Guide

## Quick Start

```python
import spprof

# Start profiling
spprof.start(interval_ms=10)

# Run your code
do_work()

# Stop and get profile
profile = spprof.stop()

# Save for visualization
profile.save("profile.json")  # Open in https://speedscope.app
```

## API Reference

### Core Functions

#### `spprof.start(interval_ms=10, output_path=None, memory_limit_mb=100)`

Start CPU profiling.

**Parameters:**
- `interval_ms` (int): Sampling interval in milliseconds. Default 10ms.
  - Lower values = more samples = more accuracy = more overhead
  - Recommended: 10ms for most cases, 1ms for short profiles
- `output_path` (Path | str | None): Auto-save path when `stop()` is called
- `memory_limit_mb` (int): Maximum memory for sample buffer. Default 100MB.

**Raises:**
- `RuntimeError`: If profiling is already active
- `ValueError`: If `interval_ms < 1`

```python
# Basic usage
spprof.start()

# High-frequency sampling for short profiles
spprof.start(interval_ms=1)

# Auto-save on stop
spprof.start(output_path="profile.json")
```

#### `spprof.stop() -> Profile`

Stop profiling and return results.

**Returns:** `Profile` object containing all samples.

**Raises:** `RuntimeError` if profiling is not active.

```python
profile = spprof.stop()
print(f"Collected {len(profile.samples)} samples")
```

#### `spprof.is_active() -> bool`

Check if profiling is currently running.

```python
if not spprof.is_active():
    spprof.start()
```

#### `spprof.stats() -> ProfilerStats | None`

Get current profiling statistics.

```python
stats = spprof.stats()
if stats:
    print(f"Samples: {stats.collected_samples}")
    print(f"Dropped: {stats.dropped_samples}")
```

### Context Manager

```python
with spprof.Profiler(interval_ms=5) as p:
    do_work()

p.profile.save("profile.json")
```

### Decorator

```python
@spprof.profile(interval_ms=10, output_path="func_profile.json")
def expensive_function():
    # This function will be profiled every time it's called
    pass
```

### Multi-Threading

For multi-threaded applications, register threads to ensure they're sampled:

```python
import threading
import spprof

def worker():
    # Register this thread for profiling
    spprof.register_thread()
    try:
        do_work()
    finally:
        spprof.unregister_thread()

spprof.start()

threads = [threading.Thread(target=worker) for _ in range(4)]
for t in threads:
    t.start()
for t in threads:
    t.join()

profile = spprof.stop()
```

Or use the context manager:

```python
def worker():
    with spprof.ThreadProfiler():
        do_work()
```

### Native Stack Unwinding

Capture C/C++ frames alongside Python frames:

```python
# Check if available
if spprof.native_unwinding_available():
    spprof.set_native_unwinding(True)

spprof.start()
# Profile code with C extensions
profile = spprof.stop()
```

## Output Formats

### Speedscope (JSON)

Interactive visualization at https://speedscope.app

```python
profile.save("profile.json", format="speedscope")
# Or
data = profile.to_speedscope()
```

### Collapsed Stack (FlameGraph)

For use with Brendan Gregg's FlameGraph tools:

```python
profile.save("profile.collapsed", format="collapsed")
# Or
text = profile.to_collapsed()
```

Generate SVG flame graph:
```bash
flamegraph.pl profile.collapsed > profile.svg
```

## Data Classes

### Profile

```python
@dataclass
class Profile:
    start_time: datetime
    end_time: datetime
    interval_ms: int
    samples: list[Sample]
    dropped_count: int
    python_version: str
    platform: str
```

### Sample

```python
@dataclass
class Sample:
    timestamp_ns: int      # Nanoseconds since profiling started
    thread_id: int         # OS thread ID
    thread_name: str | None
    frames: Sequence[Frame]  # Call stack (bottom to top)
```

### Frame

```python
@dataclass
class Frame:
    function_name: str
    filename: str
    lineno: int
    is_native: bool  # True for C extension frames
```

## Best Practices

### 1. Choose the Right Sampling Interval

| Use Case | Interval | Notes |
|----------|----------|-------|
| Production | 100ms | Minimal overhead |
| Development | 10ms | Good balance |
| Short functions | 1ms | Catches fast code |
| Micro-benchmarks | 1ms | Maximum detail |

### 2. Profile Representative Workloads

Profile with realistic data and load patterns:

```python
# Good: Profile actual workload
with spprof.Profiler(output_path="real_profile.json"):
    process_actual_data()

# Bad: Profile with tiny test data
with spprof.Profiler():
    process_one_item()  # Not representative
```

### 3. Handle Long-Running Profiles

For profiles longer than a few minutes, use memory limits:

```python
spprof.start(
    interval_ms=100,  # Lower frequency
    memory_limit_mb=50,  # Limit memory
)
```

### 4. Filter Output

When analyzing, focus on relevant code:

```python
profile = spprof.stop()

# Filter to your code only
my_samples = [
    s for s in profile.samples
    if any("myapp" in f.filename for f in s.frames)
]
```

## Troubleshooting

For comprehensive troubleshooting, see the [Troubleshooting Guide](TROUBLESHOOTING.md).

### Quick Fixes

#### "Profiler already running"

```python
# Check before starting
if not spprof.is_active():
    spprof.start()
```

#### No samples collected

1. Check if native extension loaded: `spprof._HAS_NATIVE`
2. Verify workload runs long enough (at least 10x interval)
3. Check for errors in `profile.dropped_count`
4. For I/O-bound code on Linux, note that sleeping threads don't generate samples (CPU-time sampling)

#### High dropped sample count

```python
# Increase memory or reduce frequency
spprof.start(interval_ms=10, memory_limit_mb=200)
```

#### High overhead

1. Increase sampling interval (e.g., 10ms → 100ms)
2. Disable native unwinding: `spprof.set_native_unwinding(False)`
3. Check if resolver cache is effective

#### Missing thread samples (Linux)

Register threads explicitly:
```python
spprof.register_thread()  # Call from each thread
```

Or use the context manager:
```python
with spprof.ThreadProfiler():
    do_work()
```

#### Container permission issues

spprof falls back to wall-time sampling when CPU-time timers are restricted. For full support:
```bash
docker run --security-opt seccomp=unconfined myapp
```

## Platform Notes

### Linux

- Best support with per-thread CPU sampling
- Uses `timer_create` with `SIGEV_THREAD_ID`
- Each thread needs explicit registration
- **Free-threading safe**: Python 3.13+ with `--disable-gil` is supported via speculative capture with validation

### macOS

- All threads sampled automatically via Mach thread suspension
- Uses `thread_suspend()`/`thread_resume()` for safe frame capture
- Thread registration is a no-op
- **Free-threading safe**: Full support for Python 3.13+ with `--disable-gil`

### Windows

- Uses timer queue with GIL acquisition
- All threads sampled automatically
- Slightly higher overhead than Unix


