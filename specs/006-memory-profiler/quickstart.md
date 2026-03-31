# Quickstart: Memory Allocation Profiler

**Feature**: 006-memory-profiler  
**Date**: December 3, 2024

---

## Overview

The spprof memory profiler provides production-grade heap profiling for Python applications using statistical sampling. It captures memory allocations from Python code, C extensions, and native libraries with less than 0.1% CPU overhead.

---

## Installation

The memory profiler is included with spprof:

```bash
pip install spprof
```

---

## Basic Usage

### Quick Profile

```python
import spprof.memprof as memprof

# Start profiling
memprof.start()

# Your code here
import numpy as np
data = np.random.randn(10000, 10000)  # ~800 MB

# Get results
snapshot = memprof.get_snapshot()
print(f"Estimated heap: {snapshot.estimated_heap_bytes / 1e9:.2f} GB")

# Stop profiling
memprof.stop()
```

### Context Manager

```python
import spprof.memprof as memprof

with memprof.MemoryProfiler() as mp:
    # Your code here
    data = [i ** 2 for i in range(10_000_000)]

# Access results after the block
mp.snapshot.save("memory_profile.json")
print(f"Top allocators:")
for site in mp.snapshot.top_allocators(5):
    print(f"  {site['function']}: {site['estimated_bytes'] / 1e6:.1f} MB")
```

---

## Configuration

### Sampling Rate

The sampling rate controls the trade-off between accuracy and overhead:

| Rate | Samples/sec* | Overhead | Use Case |
|------|--------------|----------|----------|
| 64 KB | ~1600 | ~0.8% | Development, debugging |
| 256 KB | ~400 | ~0.2% | Testing, CI |
| **512 KB** (default) | ~200 | **~0.1%** | **Production** |
| 1 MB | ~100 | ~0.05% | Long-running profiles |

*At 100 MB/s allocation rate

```python
# More accurate (higher overhead)
memprof.start(sampling_rate_kb=64)

# Production-safe (default)
memprof.start(sampling_rate_kb=512)

# Minimal overhead
memprof.start(sampling_rate_kb=1024)
```

---

## Working with Snapshots

### Get Top Allocators

```python
snapshot = memprof.get_snapshot()

# Top 10 allocation sites by memory
for site in snapshot.top_allocators(10):
    print(f"{site['function']} ({site['file']}:{site['line']})")
    print(f"  {site['estimated_bytes'] / 1e6:.1f} MB across {site['sample_count']} samples")
```

### Save for Visualization

```python
# Speedscope format (recommended)
snapshot.save("memory_profile.json", format="speedscope")

# Collapsed format (for FlameGraph)
snapshot.save("memory_profile.collapsed", format="collapsed")
```

Then open `memory_profile.json` at [speedscope.app](https://speedscope.app).

### Check Data Quality

```python
snapshot = memprof.get_snapshot()

# Low sample count warning
if snapshot.live_samples < 100:
    print(f"⚠️ Only {snapshot.live_samples} samples - results may have high variance")

# Frame pointer health
health = snapshot.frame_pointer_health
print(f"Stack capture confidence: {health.confidence}")
if health.recommendation:
    print(f"  Recommendation: {health.recommendation}")
```

---

## Combined CPU + Memory Profiling

Both profilers can run simultaneously:

```python
import spprof
import spprof.memprof as memprof

# Start both
spprof.start(interval_ms=10)
memprof.start(sampling_rate_kb=512)

# Your workload
run_application()

# Collect results
cpu_profile = spprof.stop()
mem_snapshot = memprof.get_snapshot()
memprof.stop()

# Save both
cpu_profile.save("cpu_profile.json")
mem_snapshot.save("mem_profile.json")
```

---

## Statistics and Diagnostics

```python
stats = memprof.get_stats()

print(f"Total samples: {stats.total_samples}")
print(f"Live samples: {stats.live_samples}")
print(f"Freed samples: {stats.freed_samples}")
print(f"Unique stacks: {stats.unique_stacks}")
print(f"Estimated heap: {stats.estimated_heap_bytes / 1e6:.1f} MB")
print(f"Heap map load: {stats.heap_map_load_percent:.1f}%")
```

---

## Linux-Specific Usage

On Linux, use LD_PRELOAD for complete native allocation tracking:

```bash
# Build the interposition library (if not pre-built)
cd spprof && make libspprof_alloc.so

# Run with profiler enabled
LD_PRELOAD=./libspprof_alloc.so python my_script.py
```

Without LD_PRELOAD, only Python-visible allocations are tracked.

---

## macOS Notes

On macOS, the profiler uses the official `malloc_logger` callback and doesn't require LD_PRELOAD. All allocations are automatically tracked.

---

## Common Patterns

### Profile a Function

```python
def profile_function(func, *args, **kwargs):
    """Profile memory usage of a function call."""
    import spprof.memprof as memprof
    
    memprof.start()
    result = func(*args, **kwargs)
    snapshot = memprof.get_snapshot()
    memprof.stop()
    
    print(f"Peak estimated heap: {snapshot.estimated_heap_bytes / 1e6:.1f} MB")
    return result, snapshot
```

### Monitor Memory Over Time

```python
import time
import spprof.memprof as memprof

memprof.start()

while running:
    process_batch()
    
    # Periodic snapshot
    snapshot = memprof.get_snapshot()
    print(f"[{time.time():.0f}] Heap: {snapshot.estimated_heap_bytes / 1e6:.1f} MB")
    
    time.sleep(60)

memprof.stop()
```

### Compare Before/After

```python
import spprof.memprof as memprof

memprof.start()

# Baseline
baseline = memprof.get_snapshot()
print(f"Baseline: {baseline.estimated_heap_bytes / 1e6:.1f} MB")

# Operation
load_large_dataset()

# After
after = memprof.get_snapshot()
print(f"After: {after.estimated_heap_bytes / 1e6:.1f} MB")
print(f"Delta: {(after.estimated_heap_bytes - baseline.estimated_heap_bytes) / 1e6:.1f} MB")

memprof.stop()
```

---

## Troubleshooting

### Low Sample Count

If you see few samples, the profiling window may be too short or allocation rate too low:

```python
# Run longer
time.sleep(10)  # Wait for more allocations

# Or lower sampling rate
memprof.start(sampling_rate_kb=64)  # 8x more samples
```

### Missing Native Frames

If native stack traces are truncated:

```bash
# Rebuild C extensions with frame pointers
CFLAGS="-fno-omit-frame-pointer" pip install --no-binary :all: numpy
```

### High Overhead

If overhead is too high:

```python
# Increase sampling rate (fewer samples)
memprof.start(sampling_rate_kb=1024)  # Half the default samples
```

---

## Next Steps

- [API Reference](contracts/python-api.md) - Complete API documentation
- [Technical Details](research.md) - Implementation decisions
- [Data Model](data-model.md) - Data structure definitions

