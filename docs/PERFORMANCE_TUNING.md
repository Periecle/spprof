# spprof Performance Tuning Guide

This guide covers how to configure spprof for optimal performance across different scenarios, including interval selection, memory management, native unwinding tradeoffs, and platform-specific considerations.

## Table of Contents

1. [Sampling Interval Selection](#sampling-interval-selection)
2. [Memory Management](#memory-management)
3. [Native Unwinding Tradeoffs](#native-unwinding-tradeoffs)
4. [Platform-Specific Considerations](#platform-specific-considerations)
5. [Overhead Estimation](#overhead-estimation)
6. [Long-Running Profiles](#long-running-profiles)
7. [Multi-Threading Optimization](#multi-threading-optimization)
8. [Troubleshooting Performance Issues](#troubleshooting-performance-issues)

---

## Sampling Interval Selection

The sampling interval (`interval_ms`) is the most important tuning parameter. It controls how often the profiler captures stack samples.

### General Guidelines

| Interval | Use Case | Overhead | Accuracy |
|----------|----------|----------|----------|
| 1ms | Short-lived functions, micro-benchmarks | High (~5-10%) | Very High |
| 5ms | Interactive profiling, development | Moderate (~1-3%) | High |
| 10ms | General development (default) | Low (~0.5-1%) | Good |
| 50ms | Long-running applications | Very Low (~0.1%) | Moderate |
| 100ms | Production monitoring | Minimal (<0.1%) | Low |

### Choosing the Right Interval

**Rule of Thumb**: Your profile duration should be at least 100x the sampling interval.

```python
# For a 1-second workload:
#   - 10ms interval → ~100 samples → Good
#   - 100ms interval → ~10 samples → May miss short functions

# For a 1-minute workload:
#   - 10ms interval → ~6000 samples → Good
#   - 100ms interval → ~600 samples → Still adequate
```

### Short-Lived Function Profiling

For functions that complete in milliseconds, use aggressive sampling:

```python
import spprof

# Profile fast function with high resolution
with spprof.Profiler(interval_ms=1) as p:
    result = fast_function()  # Completes in 50ms

# Expect ~50 samples for a 50ms function at 1ms interval
print(f"Captured {p.profile.sample_count} samples")
```

### Production Monitoring

For continuous production profiling, minimize overhead:

```python
import spprof

# Low-overhead continuous profiling
spprof.start(
    interval_ms=100,       # 100ms = 10 samples/second
    memory_limit_mb=50,    # Limit memory usage
)

# Run for extended period
import time
time.sleep(60)  # 1 minute

profile = spprof.stop()
# Expect ~600 samples over 1 minute
```

---

## Memory Management

### Understanding Memory Usage

Each sample consumes approximately:
- **Python-only**: ~3 KB per sample (128 frames × 24 bytes)
- **With native unwinding**: ~4 KB per sample (additional native frame storage)

For a 10-minute profile at 10ms interval:
```
60,000 samples × 3 KB = ~180 MB
```

### Using Memory Limits

Set appropriate memory limits to prevent OOM:

```python
import spprof

# For long-running profiles
spprof.start(
    interval_ms=10,
    memory_limit_mb=100,  # Cap at 100 MB
)
```

When the ring buffer fills:
- New samples are dropped (not crashed)
- `profile.dropped_count` indicates how many were lost
- Consider increasing interval or memory limit

### Sample Aggregation for Long Profiles

For profiles with repetitive call patterns, use aggregation:

```python
import spprof

spprof.start(interval_ms=10)
# ... long-running workload ...
profile = spprof.stop()

# Aggregate identical stacks
agg = profile.aggregate()

print(f"Original samples: {agg.total_samples}")
print(f"Unique stacks: {agg.unique_stack_count}")
print(f"Compression ratio: {agg.compression_ratio:.1f}x")
print(f"Memory reduction: {agg.memory_reduction_pct:.1f}%")

# Save aggregated profile (same formats work)
agg.save("profile.json")
```

Typical compression ratios:
- Hot loops calling same functions: 10-100x compression
- Complex branching code: 2-5x compression
- Highly dynamic code (eval, metaprogramming): <2x compression

---

## Native Unwinding Tradeoffs

Native unwinding captures C/C++ stack frames alongside Python frames, enabling mixed-mode profiling for debugging C extensions.

### When to Enable Native Unwinding

**Enable when:**
- Debugging C extension performance (NumPy, pandas, etc.)
- Profiling C code called via ctypes/cffi
- Investigating time spent in Python runtime itself

**Disable when:**
- Pure Python profiling (default)
- Production monitoring (lower overhead)
- Memory-constrained environments

### Performance Impact

| Mode | Overhead | Memory | Use Case |
|------|----------|--------|----------|
| Python-only | Low | ~3 KB/sample | General profiling |
| Native unwinding | Moderate | ~4 KB/sample | C extension debugging |

```python
import spprof

# Check availability first
if spprof.native_unwinding_available():
    spprof.set_native_unwinding(True)
    print("Native unwinding enabled")
else:
    print("Native unwinding not available on this platform")

spprof.start(interval_ms=10)
# ... profile C extension code ...
profile = spprof.stop()

# Native frames are marked in output
for sample in profile.samples[:5]:
    for frame in sample.frames:
        prefix = "[native] " if frame.is_native else ""
        print(f"  {prefix}{frame.function_name}")
```

### Platform Support

| Platform | Native Unwinding Method | Notes |
|----------|------------------------|-------|
| Linux | libunwind | Best support, most accurate |
| macOS | backtrace() | Good support, some symbol limitations |
| Windows | CaptureStackBackTrace | Limited C runtime support |

---

## Platform-Specific Considerations

### Linux

**Strengths:**
- Best overall support with `timer_create`
- Per-thread CPU time sampling (most accurate)
- Each thread can have independent timer

**Configuration:**
```python
import spprof

# Register worker threads for per-thread sampling
def worker():
    spprof.register_thread()
    try:
        # ... do work ...
        pass
    finally:
        spprof.unregister_thread()

spprof.start(interval_ms=10)
threads = [threading.Thread(target=worker) for _ in range(4)]
# ...
```

**Tuning Tips:**
- Thread registration is required for accurate per-thread profiling
- Consider CLOCK_THREAD_CPUTIME_ID semantics: sleeping threads don't generate samples
- For IO-bound workloads, wall-clock profiling may be more informative

### macOS

**Strengths:**
- All threads sampled automatically (no registration needed)
- Good system integration

**Limitations:**
- Uses `setitimer(ITIMER_PROF)` - process-wide, not per-thread
- When main thread sleeps, other threads may not be sampled
- Very aggressive sampling (1ms) can be unstable

**Configuration:**
```python
import spprof

# No thread registration needed on macOS
spprof.start(interval_ms=10)

# All threads automatically sampled
threads = [threading.Thread(target=worker) for _ in range(4)]
# ...
```

**Tuning Tips:**
- Use 5ms+ intervals for stability
- Keep main thread active for best results
- Consider Mach-based sampling for production use

### Windows

**Strengths:**
- All threads sampled via timer queue
- Good integration with Windows debugging tools

**Limitations:**
- Slightly higher overhead than Unix (thread suspension)
- Timer resolution limited by system timer tick (~15.6ms default)

**Configuration:**
```python
import spprof

# Minimum effective interval on Windows is often ~16ms
# due to system timer resolution
spprof.start(interval_ms=16)  # Align with timer tick
```

**Tuning Tips:**
- Use `timeBeginPeriod(1)` to increase timer resolution (system-wide impact)
- Consider intervals >= 16ms for consistent sampling
- Thread suspension adds slight overhead vs Unix signal-based approach

---

## Overhead Estimation

### Built-in Overhead Estimation

```python
import spprof

spprof.start(interval_ms=10)
# ... workload ...
stats = spprof.stats()

if stats:
    print(f"Estimated overhead: {stats.overhead_estimate_pct:.2f}%")
```

### Manual Overhead Measurement

For accurate overhead measurement:

```python
import time
import spprof

def workload():
    total = 0
    for i in range(1_000_000):
        total += i
    return total

# Baseline (no profiling)
start = time.perf_counter()
for _ in range(10):
    workload()
baseline = time.perf_counter() - start

# With profiling
spprof.start(interval_ms=10)
start = time.perf_counter()
for _ in range(10):
    workload()
profiled = time.perf_counter() - start
spprof.stop()

overhead_pct = ((profiled - baseline) / baseline) * 100
print(f"Measured overhead: {overhead_pct:.2f}%")
```

### Overhead Breakdown

Typical overhead sources:
1. **Signal delivery**: ~1-5μs per sample
2. **Frame walking**: ~5-20μs per sample (depends on stack depth)
3. **Ring buffer write**: ~0.5-2μs per sample
4. **Native unwinding**: +10-50μs per sample (if enabled)

For 10ms interval: `~25μs / 10ms = 0.25%` overhead

---

## Long-Running Profiles

### Streaming vs Batch Processing

For very long profiles (hours), use streaming:

```python
import spprof
import json

# Start with memory limit
spprof.start(interval_ms=100, memory_limit_mb=50)

# Periodically drain samples
import time
all_samples = []

for _ in range(60):  # 1 hour in 1-minute chunks
    time.sleep(60)
    stats = spprof.stats()
    if stats:
        print(f"Samples: {stats.collected_samples}, Dropped: {stats.dropped_samples}")
    # Samples accumulate in ring buffer

profile = spprof.stop()

# Use aggregation to reduce final memory
agg = profile.aggregate()
agg.save("long_running_profile.json")
```

### Reducing Sample Rate for Long Profiles

```python
import spprof

# For 1-hour profile, 100ms interval gives ~36,000 samples
# Much more manageable than 10ms (360,000 samples)
spprof.start(
    interval_ms=100,
    memory_limit_mb=100,
)
```

---

## Multi-Threading Optimization

### Linux: Per-Thread Timers

```python
import spprof
import threading

def optimized_worker():
    """Worker with explicit registration."""
    # Register immediately when thread starts
    spprof.register_thread()
    try:
        # Main work
        result = expensive_computation()
        return result
    finally:
        # Always unregister
        spprof.unregister_thread()

# Or use context manager
def worker_with_context():
    with spprof.ThreadProfiler():
        return expensive_computation()
```

### Thread Pool Considerations

For thread pools (concurrent.futures, multiprocessing):

```python
import spprof
from concurrent.futures import ThreadPoolExecutor

def task(x):
    # Register on first call in this thread
    if not hasattr(task, '_registered'):
        spprof.register_thread()
        task._registered = True
    
    return x ** 2

spprof.start(interval_ms=10)

with ThreadPoolExecutor(max_workers=4) as executor:
    results = list(executor.map(task, range(1000)))

profile = spprof.stop()
```

---

## Troubleshooting Performance Issues

### High Overhead

**Symptoms:**
- Application noticeably slower during profiling
- Overhead > 5%

**Solutions:**
1. Increase sampling interval
2. Disable native unwinding
3. Reduce stack depth (avoid deep recursion during profiling)

```python
# Before: High overhead
spprof.start(interval_ms=1)  # Too aggressive
spprof.set_native_unwinding(True)  # Adds overhead

# After: Lower overhead
spprof.start(interval_ms=10)  # More reasonable
spprof.set_native_unwinding(False)
```

### Dropped Samples

**Symptoms:**
- `profile.dropped_count > 0`
- Missing data in flame graphs

**Solutions:**
1. Increase memory limit
2. Increase sampling interval (fewer samples)
3. Use aggregation for analysis

```python
# Check for drops
profile = spprof.stop()
if profile.dropped_count > 0:
    print(f"Warning: {profile.dropped_count} samples dropped")
    print("Consider: increasing memory_limit_mb or interval_ms")
```

### Missing Thread Samples (Linux)

**Symptoms:**
- Worker threads not appearing in profile
- Only main thread visible

**Solutions:**
1. Ensure `register_thread()` called from each worker
2. Use `ThreadProfiler` context manager
3. Verify threads are CPU-active (sleeping threads don't sample)

```python
# Verify thread registration
import spprof

def debug_worker():
    result = spprof.register_thread()
    print(f"Thread registered: {result}")
    # ... work ...
    spprof.unregister_thread()
```

### No Samples Collected

**Symptoms:**
- `profile.sample_count == 0`

**Possible Causes:**
1. Workload too short (< interval_ms)
2. Profiler not started (`spprof.is_active() == False`)
3. Native extension not loaded

```python
import spprof

# Diagnostic checks
print(f"Native extension loaded: {spprof._HAS_NATIVE}")
print(f"Profiler active: {spprof.is_active()}")

spprof.start(interval_ms=1)
# Ensure sufficient work
import time
time.sleep(0.1)  # 100ms minimum
profile = spprof.stop()

print(f"Samples collected: {profile.sample_count}")
```

---

## Quick Reference

| Scenario | Interval | Memory | Native Unwinding |
|----------|----------|--------|-----------------|
| Quick debugging | 1-5ms | Default | Off |
| Development | 10ms | Default | Off |
| C extension debugging | 10ms | Default | On |
| Production monitoring | 100ms | 50MB | Off |
| Long-running (hours) | 100ms | 100MB | Off |

### Performance Checklist

- [ ] Interval appropriate for workload duration
- [ ] Memory limit set for long profiles
- [ ] Native unwinding off unless debugging C code
- [ ] Threads registered (Linux)
- [ ] Aggregation used for analysis of large profiles


