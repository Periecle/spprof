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

---

## Memory Profiling

spprof includes a memory allocation profiler that uses statistical sampling to track heap allocations with ultra-low overhead (<0.1% CPU).

### Quick Start

```python
import spprof.memprof as memprof

# Start profiling
memprof.start(sampling_rate_kb=512)  # Default: sample ~every 512KB

# Run your code
do_work()

# Get snapshot of live allocations
snapshot = memprof.get_snapshot()
print(f"Estimated heap: {snapshot.estimated_heap_bytes / 1e6:.1f} MB")

# Stop profiling
memprof.stop()
```

### Memory Profiler API

#### `memprof.start(sampling_rate_kb=512)`

Start memory profiling.

**Parameters:**
- `sampling_rate_kb` (int): Average kilobytes between samples. Default 512KB.
  - Lower = more samples = more accuracy = more overhead
  - Recommended: 512KB for production, 64KB for debugging

**Raises:**
- `RuntimeError`: If profiler is already running
- `ValueError`: If `sampling_rate_kb < 1`

```python
# Production (minimal overhead)
memprof.start(sampling_rate_kb=512)

# Development (more accuracy)
memprof.start(sampling_rate_kb=64)
```

#### `memprof.stop()`

Stop memory profiling.

Note: This stops tracking new allocations but continues tracking frees
to prevent "fake leaks" from appearing in snapshots.

#### `memprof.get_snapshot() -> HeapSnapshot`

Get snapshot of currently live (unfreed) allocations.

```python
snapshot = memprof.get_snapshot()
print(f"Live samples: {snapshot.live_samples}")
print(f"Estimated heap: {snapshot.estimated_heap_bytes / 1e6:.1f} MB")

# Get top allocation sites
for site in snapshot.top_allocators(5):
    print(f"{site['function']}: {site['estimated_bytes'] / 1e6:.1f} MB")
```

#### `memprof.get_stats() -> MemProfStats`

Get profiler statistics.

```python
stats = memprof.get_stats()
print(f"Total samples: {stats.total_samples}")
print(f"Live: {stats.live_samples}, Freed: {stats.freed_samples}")
print(f"Heap map load: {stats.heap_map_load_percent:.1f}%")
```

#### `memprof.shutdown()`

Shutdown profiler completely (one-way operation).

**Warning:** After shutdown, `start()` will raise `RuntimeError`.

### Context Manager

```python
with memprof.MemoryProfiler(sampling_rate_kb=256) as mp:
    do_work()

# Snapshot available after exit
mp.snapshot.save("memory_profile.json")
```

### Saving Profiles

```python
# Speedscope format (recommended)
snapshot.save("profile.json", format="speedscope")

# Collapsed format (for FlameGraph)
snapshot.save("profile.collapsed", format="collapsed")
```

View profiles at https://speedscope.app

### Combined CPU + Memory Profiling

Both profilers can run simultaneously:

```python
import spprof
import spprof.memprof as memprof

# Start both
spprof.start(interval_ms=10)
memprof.start(sampling_rate_kb=512)

# Run workload
do_work()

# Get both results
cpu_profile = spprof.stop()
mem_snapshot = memprof.get_snapshot()
memprof.stop()

# Save both
cpu_profile.save("cpu.json")
mem_snapshot.save("memory.json")
```

### Memory Profiler Data Classes

#### HeapSnapshot

```python
@dataclass
class HeapSnapshot:
    samples: List[AllocationSample]  # Live allocations
    total_samples: int               # All samples (live + freed)
    live_samples: int                # Currently live
    estimated_heap_bytes: int        # Estimated heap size
    timestamp_ns: int                # When snapshot was taken
    frame_pointer_health: FramePointerHealth
```

#### AllocationSample

```python
@dataclass
class AllocationSample:
    address: int              # Allocation address
    size: int                 # Actual size in bytes
    weight: int               # Sampling weight (= sampling_rate)
    estimated_bytes: int      # Contribution to heap estimate
    timestamp_ns: int         # When allocated
    lifetime_ns: Optional[int] # Duration if freed
    stack: List[StackFrame]   # Call stack
```

#### MemProfStats

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

### Memory Profiler Platform Notes

#### macOS

- Uses `malloc_logger` callback (official Apple API)
- All allocations captured automatically
- No special setup required

#### Linux

For complete allocation tracking including C extensions:

```bash
# Build the interposition library
# (included with spprof)

# Run with LD_PRELOAD
LD_PRELOAD=/path/to/libspprof_alloc.so python myapp.py
```

Without LD_PRELOAD, only allocations visible to Python are tracked.

#### Windows

- Experimental support
- Uses Detours for allocation hooks
- Some allocations may not be captured

### Memory Profiler Best Practices

1. **Choose the Right Sampling Rate**

| Use Case | Rate | Overhead |
|----------|------|----------|
| Production | 512KB | <0.1% |
| Testing | 256KB | ~0.2% |
| Debugging | 64KB | ~0.8% |

2. **Check Sample Count**

```python
snapshot = memprof.get_snapshot()
if snapshot.live_samples < 100:
    print("⚠️ Low sample count - consider lower sampling rate")
```

3. **Monitor Frame Pointer Health**

```python
health = snapshot.frame_pointer_health
print(f"Confidence: {health.confidence}")
if health.recommendation:
    print(health.recommendation)
```

4. **For Long-Running Profiles**

Take periodic snapshots instead of one large profile:

```python
memprof.start(sampling_rate_kb=1024)  # Higher rate = less overhead

for batch in batches:
    process(batch)
    
    # Periodic snapshot
    snap = memprof.get_snapshot()
    log_heap_size(snap.estimated_heap_bytes)
```


