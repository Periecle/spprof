# spprof Troubleshooting Guide

This guide covers common issues you may encounter when using spprof and how to resolve them.

## Table of Contents

1. [No Samples Captured](#1-no-samples-captured)
2. [High Dropped Sample Count](#2-high-dropped-sample-count)
3. [Container Permission Issues](#3-container-permission-issues)
4. [Missing Thread Samples](#4-missing-thread-samples)
5. [Profiler Already Running](#5-profiler-already-running)
6. [Native Extension Not Loaded](#6-native-extension-not-loaded)
7. [High Overhead](#7-high-overhead)
8. [Empty or Sparse Flame Graphs](#8-empty-or-sparse-flame-graphs)
9. [Free-Threaded Python Issues](#9-free-threaded-python-issues)

---

## 1. No Samples Captured

### Symptoms

- `profile.sample_count == 0` after stopping the profiler
- Empty flame graph output
- `spprof.stop()` returns a Profile with no samples

### Common Causes and Solutions

#### 1.1 Workload Too Short

The most common cause is that your workload completes faster than the sampling interval.

**Rule of Thumb**: Your workload should run for at least **10x the sampling interval** to capture meaningful samples.

| Interval | Minimum Workload Duration |
|----------|--------------------------|
| 1ms | 10ms |
| 10ms (default) | 100ms |
| 100ms | 1 second |

**Solution**: Reduce the sampling interval for short-lived work:

```python
import spprof

# For fast functions (< 100ms), use aggressive sampling
spprof.start(interval_ms=1)
fast_function()  # Must run > 10ms
profile = spprof.stop()

print(f"Samples: {profile.sample_count}")
```

#### 1.2 Code Running in C Extensions

If your workload primarily executes in C extensions (e.g., NumPy operations), Python frames may not be visible because the code spends most time in native code.

**Solution**: Enable native unwinding to capture C frames:

```python
import spprof

if spprof.native_unwinding_available():
    spprof.set_native_unwinding(True)

spprof.start(interval_ms=1)
numpy_heavy_computation()
profile = spprof.stop()
```

#### 1.3 I/O-Bound or Sleeping Code

On Linux, spprof uses `CLOCK_THREAD_CPUTIME_ID`, which only counts CPU time. Sleeping or I/O-waiting threads don't generate samples.

**Diagnostic**:

```python
import spprof
import time

spprof.start(interval_ms=10)
time.sleep(1)  # No CPU time consumed = no samples!
profile = spprof.stop()

print(f"Samples: {profile.sample_count}")  # Likely 0
```

**Solution**: This is expected behavior. The profiler measures CPU time, not wall-clock time. Profile CPU-bound workloads instead, or ensure your workload has active computation.

#### 1.4 Native Extension Not Loaded

Check if the C extension loaded correctly:

```python
import spprof

print(f"Native extension loaded: {spprof._HAS_NATIVE}")

if not spprof._HAS_NATIVE:
    print("Running in pure Python fallback mode")
    print("Install with: pip install spprof --force-reinstall")
```

---

## 2. High Dropped Sample Count

### Symptoms

- `profile.dropped_count` is significantly higher than 0
- Warning messages about dropped samples
- Incomplete or misleading flame graphs

### Understanding Dropped Samples

Samples are "dropped" when the ring buffer fills up faster than the resolver can drain it. This happens when:

1. **High sampling frequency** (low `interval_ms`) generates samples faster than they can be processed
2. **Memory limit reached** (`memory_limit_mb` exceeded)
3. **Long-running profiles** without sufficient buffer space

### Solutions

#### 2.1 Check Drop Count

```python
import spprof

spprof.start(interval_ms=10)
# ... long workload ...
profile = spprof.stop()

if profile.dropped_count > 0:
    drop_rate = profile.dropped_count / (profile.sample_count + profile.dropped_count) * 100
    print(f"⚠️ Dropped {profile.dropped_count} samples ({drop_rate:.1f}%)")
    
    if drop_rate > 5:
        print("Consider increasing interval_ms or memory_limit_mb")
```

#### 2.2 Increase Memory Limit

```python
import spprof

# For long-running profiles, increase buffer size
spprof.start(
    interval_ms=10,
    memory_limit_mb=200,  # Default is 100MB
)
```

#### 2.3 Reduce Sampling Frequency

```python
import spprof

# For very long profiles (hours), reduce sampling rate
spprof.start(
    interval_ms=100,  # 10 samples/second instead of 100
    memory_limit_mb=100,
)
```

#### 2.4 Use Aggregation for Analysis

Even with dropped samples, aggregation can provide useful insights:

```python
profile = spprof.stop()

# Aggregate reduces memory and works well with partial data
agg = profile.aggregate()
print(f"Captured {agg.total_samples} samples across {agg.unique_stack_count} unique stacks")
agg.save("profile.json")
```

#### 2.5 Memory Usage Estimation

Estimate required memory before profiling:

```python
# Each sample ≈ 3-4 KB
# 10ms interval = 100 samples/second per active thread
# 4 threads × 100 samples/sec × 60 seconds = 24,000 samples
# 24,000 × 4 KB ≈ 96 MB

expected_samples = num_threads * (1000 / interval_ms) * duration_seconds
expected_memory_mb = expected_samples * 4 / 1024

print(f"Expected memory usage: {expected_memory_mb:.0f} MB")
```

---

## 3. Container Permission Issues

### Symptoms

- Timer creation failures
- Profiler silently falls back to wall-time sampling
- Different behavior in containers vs bare metal
- Error messages about `timer_create` or `CLOCK_THREAD_CPUTIME_ID`

### Understanding Container Limitations

Containers often run with restricted syscalls due to:

1. **seccomp profiles** blocking timer-related syscalls
2. **cgroups v1** limitations on CPU time access
3. **Docker's default security settings**

### Solutions

#### 3.1 Check Current Sampling Mode

spprof automatically falls back to wall-time sampling when CPU-time timers fail:

```python
import spprof

spprof.start(interval_ms=10)
stats = spprof.stats()

# Check if running in fallback mode (implementation specific)
# Wall-time fallback is automatic and transparent
```

#### 3.2 Docker: Run with Extended Permissions

```bash
# Option 1: Disable seccomp (development only)
docker run --security-opt seccomp=unconfined myapp

# Option 2: Use a custom seccomp profile allowing timer_create
docker run --security-opt seccomp=custom-profile.json myapp

# Option 3: Add SYS_PTRACE capability (some configurations)
docker run --cap-add SYS_PTRACE myapp
```

**Note**: spprof does **not** require `SYS_PTRACE` for normal operation. It only uses in-process sampling via timers and signals.

#### 3.3 Kubernetes: Configure Security Context

```yaml
apiVersion: v1
kind: Pod
spec:
  containers:
  - name: myapp
    securityContext:
      # Allow timer_create syscall
      seccompProfile:
        type: Unconfined  # Or use a custom profile
```

#### 3.4 Custom seccomp Profile

Create a profile that allows timer-related syscalls:

```json
{
  "defaultAction": "SCMP_ACT_ERRNO",
  "architectures": ["SCMP_ARCH_X86_64"],
  "syscalls": [
    {
      "names": [
        "timer_create",
        "timer_settime",
        "timer_delete",
        "timer_gettime",
        "clock_gettime"
      ],
      "action": "SCMP_ACT_ALLOW"
    }
  ]
}
```

#### 3.5 Wall-Time Fallback Behavior

When CPU-time timers aren't available, spprof automatically uses `CLOCK_MONOTONIC` (wall-time):

| Mode | Measures | Best For |
|------|----------|----------|
| CPU-time (default) | Active CPU execution | CPU-bound profiling |
| Wall-time (fallback) | Elapsed time | I/O-bound or container environments |

Wall-time mode will sample sleeping threads, which may not be desired for CPU profiling but provides broader coverage.

#### 3.6 Verify Container Works

Test script to verify spprof works in your container:

```python
#!/usr/bin/env python3
"""Test spprof container compatibility."""
import spprof
import time

def cpu_work():
    """CPU-bound work to generate samples."""
    total = 0
    for i in range(1_000_000):
        total += i * i
    return total

print(f"spprof version: {spprof.__version__}")
print(f"Native extension: {spprof._HAS_NATIVE}")

spprof.start(interval_ms=5)
cpu_work()
profile = spprof.stop()

print(f"Samples collected: {profile.sample_count}")
print(f"Dropped samples: {profile.dropped_count}")

if profile.sample_count > 0:
    print("✅ spprof is working correctly")
else:
    print("❌ No samples captured - check container permissions")
```

---

## 4. Missing Thread Samples

### Symptoms (Linux)

- Only the main thread appears in the profile
- Worker threads not captured
- Thread pool work not visible

### Cause

On Linux, spprof uses per-thread timers with `SIGEV_THREAD_ID`. Each thread must be explicitly registered to receive its own profiling timer.

### Solutions

#### 4.1 Register Worker Threads

```python
import threading
import spprof

def worker():
    # Register this thread for profiling
    spprof.register_thread()
    try:
        do_work()
    finally:
        # Clean up when done
        spprof.unregister_thread()

spprof.start()

threads = [threading.Thread(target=worker) for _ in range(4)]
for t in threads:
    t.start()
for t in threads:
    t.join()

profile = spprof.stop()
```

#### 4.2 Use ThreadProfiler Context Manager

```python
import threading
import spprof

def worker():
    with spprof.ThreadProfiler():
        do_work()

spprof.start()
threads = [threading.Thread(target=worker) for _ in range(4)]
# ...
```

#### 4.3 Thread Pool Pattern

For thread pools, register on first task execution:

```python
from concurrent.futures import ThreadPoolExecutor
import spprof

_thread_registered = threading.local()

def task(x):
    # Register once per thread
    if not getattr(_thread_registered, 'done', False):
        spprof.register_thread()
        _thread_registered.done = True
    return x ** 2

spprof.start()
with ThreadPoolExecutor(max_workers=4) as executor:
    results = list(executor.map(task, range(1000)))
profile = spprof.stop()
```

### Platform Notes

| Platform | Thread Registration |
|----------|-------------------|
| Linux | **Required** for worker threads |
| macOS | Not needed (all threads sampled via Mach) |
| Windows | Not needed (all threads sampled via timer queue) |

---

## 5. Profiler Already Running

### Symptoms

```
RuntimeError: Profiler already running
```

### Cause

Calling `spprof.start()` when profiling is already active.

### Solutions

#### 5.1 Check Before Starting

```python
import spprof

if not spprof.is_active():
    spprof.start()
```

#### 5.2 Stop Existing Session

```python
import spprof

if spprof.is_active():
    # Stop and discard existing profile
    _ = spprof.stop()

spprof.start()
```

#### 5.3 Use Context Manager (Recommended)

Context managers handle cleanup automatically:

```python
import spprof

with spprof.Profiler() as p:
    do_work()

# Profiler is automatically stopped here
p.profile.save("output.json")
```

---

## 6. Native Extension Not Loaded

### Symptoms

- `spprof._HAS_NATIVE` is `False`
- Pure Python fallback mode (limited functionality)
- Warning about missing C extension

### Diagnostic

```python
import spprof

print(f"Native: {spprof._HAS_NATIVE}")
print(f"Module: {spprof._native}")
```

### Solutions

#### 6.1 Reinstall spprof

```bash
pip uninstall spprof
pip install spprof --no-cache-dir
```

#### 6.2 Install from Source

```bash
pip install git+https://github.com/Periecle/spprof.git
```

#### 6.3 Check Build Dependencies

On Linux, ensure you have:

```bash
# Debian/Ubuntu
sudo apt-get install python3-dev build-essential

# Fedora/RHEL
sudo dnf install python3-devel gcc
```

#### 6.4 Verify Import

```python
try:
    import spprof._native as native
    print("Native extension loaded successfully")
    print(dir(native))
except ImportError as e:
    print(f"Import failed: {e}")
```

---

## 7. High Overhead

### Symptoms

- Application noticeably slower during profiling
- Overhead > 5%
- System becomes unresponsive

### Solutions

#### 7.1 Increase Sampling Interval

```python
# Before: High overhead
spprof.start(interval_ms=1)  # 1000 samples/second

# After: Lower overhead  
spprof.start(interval_ms=10)  # 100 samples/second (default)

# Production monitoring
spprof.start(interval_ms=100)  # 10 samples/second
```

#### 7.2 Disable Native Unwinding

Native stack unwinding adds 10-50μs per sample:

```python
# Disable native unwinding if not needed
spprof.set_native_unwinding(False)
spprof.start(interval_ms=10)
```

#### 7.3 Measure Actual Overhead

```python
import time
import spprof

def benchmark():
    total = 0
    for i in range(1_000_000):
        total += i
    return total

# Baseline
start = time.perf_counter()
for _ in range(10):
    benchmark()
baseline = time.perf_counter() - start

# With profiling
spprof.start(interval_ms=10)
start = time.perf_counter()
for _ in range(10):
    benchmark()
profiled = time.perf_counter() - start
spprof.stop()

overhead_pct = ((profiled - baseline) / baseline) * 100
print(f"Measured overhead: {overhead_pct:.2f}%")
```

### Overhead Guidelines

| Interval | Expected Overhead |
|----------|------------------|
| 1ms | 5-10% |
| 10ms | <1% |
| 100ms | <0.1% |

---

## 8. Empty or Sparse Flame Graphs

### Symptoms

- Flame graph shows very few functions
- Important code paths missing
- Profile seems incomplete

### Solutions

#### 8.1 Ensure Sufficient Samples

```python
profile = spprof.stop()

print(f"Total samples: {profile.sample_count}")
print(f"Duration: {profile.total_duration_ms:.0f}ms")
print(f"Effective rate: {profile.effective_rate_hz:.1f} Hz")

# Aim for at least 100 samples for meaningful analysis
if profile.sample_count < 100:
    print("Warning: Few samples. Run longer or decrease interval_ms")
```

#### 8.2 Profile Representative Workload

```python
# Good: Profile real workload
with spprof.Profiler() as p:
    process_real_dataset()

# Bad: Profile trivial workload
with spprof.Profiler() as p:
    process_one_item()  # Too fast to capture
```

#### 8.3 Check for Dropped Samples

```python
profile = spprof.stop()

total_expected = profile.sample_count + profile.dropped_count
if profile.dropped_count > 0:
    print(f"Lost {profile.dropped_count}/{total_expected} samples")
    print("Increase memory_limit_mb or interval_ms")
```

#### 8.4 Filter to Relevant Code

When analyzing, filter to your code:

```python
profile = spprof.stop()

# Find samples with your code
my_samples = [
    s for s in profile.samples
    if any("myproject" in f.filename for f in s.frames)
]

print(f"Samples in your code: {len(my_samples)}/{profile.sample_count}")
```

---

## 9. Free-Threaded Python Issues

### Symptoms

- Higher than expected `validation_drops` count
- Slightly fewer samples than expected on free-threaded builds
- Questions about free-threading compatibility

### Understanding Free-Threading Support

spprof supports free-threaded Python 3.13+ (`--disable-gil`) on both Linux and macOS:

| Platform | Mechanism | Safety Model |
|----------|-----------|--------------|
| Linux | Speculative capture + validation | Multi-layer validation, ~0.0005% drop rate |
| macOS | Mach thread suspension | Thread fully stopped during capture |

### Checking Free-Threading Status

```python
import spprof

# Check if running on a free-threaded build
if hasattr(spprof._native, 'free_threaded_build'):
    print(f"Free-threaded build: {spprof._native.free_threaded_build}")
    print(f"Free-threading safe: {spprof._native.free_threading_safe}")
```

### Validation Drops (Linux Free-Threading)

On Linux with free-threaded Python, some samples may be dropped due to validation failures. This is normal and expected:

```python
import spprof

spprof.start(interval_ms=10)
# ... workload with many threads ...
profile = spprof.stop()

# Check validation drops (free-threading specific)
stats = spprof.stats()
if stats:
    print(f"Collected samples: {profile.sample_count}")
    print(f"Dropped (buffer): {profile.dropped_count}")
    # Validation drops are tracked separately in extended stats
```

**Why Drops Happen:**

1. **Race window**: Frame chain updates take ~10-50ns
2. **Sampling interval**: 10ms (default)
3. **Collision probability**: ~0.0005% per sample
4. **Result**: Corrupted reads are detected and sample is safely dropped

**When to Worry:**

- Drop rate < 1%: Normal operation
- Drop rate 1-5%: Consider increasing interval
- Drop rate > 5%: Check for highly contended code paths

### Best Practices for Free-Threaded Python

```python
import spprof

# 1. Use slightly longer intervals for many-threaded workloads
spprof.start(interval_ms=20)  # Instead of 10ms

# 2. Register threads explicitly on Linux
def worker():
    spprof.register_thread()
    try:
        do_work()
    finally:
        spprof.unregister_thread()

# 3. Use aggregation to minimize impact of dropped samples
profile = spprof.stop()
agg = profile.aggregate()
agg.save("profile.json")
```

### Platform-Specific Notes

**Linux (Speculative Capture):**
- Uses atomic loads with appropriate memory ordering
- x86-64: Strong memory model, plain loads sufficient
- ARM64: Uses acquire semantics for weak memory model
- Validation checks: pointer bounds, alignment, type, cycle detection

**macOS (Mach Suspension):**
- Thread is fully suspended before frame capture
- No race conditions possible
- Zero validation drops

---

## Quick Diagnostic Script

Run this script to diagnose common issues:

```python
#!/usr/bin/env python3
"""spprof diagnostic script."""
import sys
import platform

print("=== System Info ===")
print(f"Python: {sys.version}")
print(f"Platform: {platform.system()} {platform.release()}")

print("\n=== spprof Status ===")
import spprof
print(f"Version: {spprof.__version__}")
print(f"Native extension: {spprof._HAS_NATIVE}")
print(f"Native unwinding available: {spprof.native_unwinding_available()}")
print(f"Currently active: {spprof.is_active()}")

# Check free-threading status
if spprof._HAS_NATIVE and hasattr(spprof._native, 'free_threaded_build'):
    print(f"Free-threaded build: {spprof._native.free_threaded_build}")
    print(f"Free-threading safe: {spprof._native.free_threading_safe}")

print("\n=== Quick Test ===")
def cpu_work():
    total = 0
    for i in range(500_000):
        total += i * i
    return total

spprof.start(interval_ms=1)
cpu_work()
profile = spprof.stop()

print(f"Samples collected: {profile.sample_count}")
print(f"Dropped samples: {profile.dropped_count}")
print(f"Duration: {profile.total_duration_ms:.0f}ms")

if profile.sample_count > 0:
    print("\n✅ spprof is working correctly!")
    # Show top functions
    from collections import Counter
    funcs = Counter()
    for s in profile.samples:
        if s.frames:
            funcs[s.frames[-1].function_name] += 1
    print("\nTop functions:")
    for func, count in funcs.most_common(5):
        print(f"  {func}: {count} samples")
else:
    print("\n❌ No samples captured. Check:")
    print("  1. Is native extension loaded?")
    print("  2. Is workload CPU-bound?")
    print("  3. Are there container restrictions?")
```

---

## Getting Help

If you've tried these solutions and still have issues:

1. **Check existing issues**: [GitHub Issues](https://github.com/Periecle/spprof/issues)
2. **File a new issue** with:
   - Python version and platform
   - spprof version (`spprof.__version__`)
   - Output of the diagnostic script above
   - Minimal reproduction example


