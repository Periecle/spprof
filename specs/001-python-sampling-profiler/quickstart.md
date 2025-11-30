# Quickstart: spprof Python Sampling Profiler

**Feature Branch**: `001-python-sampling-profiler`  
**Date**: 2025-11-29

---

## Installation

```bash
# From PyPI (when published)
pip install spprof

# From source
git clone https://github.com/Periecle/spprof.git
cd spprof
pip install -e .
```

### Requirements

- Python 3.9–3.14
- Linux, macOS, or Windows
- No elevated privileges required (works in restricted containers)

---

## Basic Usage

### Start/Stop API

```python
import spprof

# Start profiling with 10ms sampling interval
spprof.start(interval_ms=10)

# Run your workload
result = expensive_computation()

# Stop profiling and get results
profile = spprof.stop()

# Save to file (Speedscope format)
profile.save("profile.json")

# Open in Speedscope: https://www.speedscope.app
```

### Context Manager

```python
import spprof

with spprof.Profiler(interval_ms=5) as p:
    result = expensive_computation()

# Profile automatically saved after block
p.profile.save("profile.json")
```

### Decorator

```python
import spprof

@spprof.profile(output_path="compute.json")
def heavy_computation():
    # This function will be profiled
    result = 0
    for i in range(10_000_000):
        result += i
    return result

heavy_computation()  # Profile saved automatically
```

---

## Viewing Profiles

### Speedscope (Recommended)

1. Save profile as JSON: `profile.save("profile.json")`
2. Open https://www.speedscope.app
3. Drag and drop `profile.json`

### FlameGraph (CLI)

```bash
# Generate collapsed format
python -c "
import spprof
spprof.start()
# ... workload ...
profile = spprof.stop()
print(profile.to_collapsed())
" > profile.collapsed

# Generate SVG with FlameGraph
git clone https://github.com/brendangregg/FlameGraph.git
./FlameGraph/flamegraph.pl profile.collapsed > profile.svg
```

---

## Multi-threaded Applications

spprof automatically profiles all Python threads:

```python
import spprof
import threading

def worker(n):
    total = 0
    for i in range(n):
        total += i ** 2
    return total

spprof.start(interval_ms=5)

# Create and run threads
threads = [threading.Thread(target=worker, args=(1_000_000,)) for _ in range(4)]
for t in threads:
    t.start()
for t in threads:
    t.join()

profile = spprof.stop()
print(f"Collected {len(profile.samples)} samples from {len(set(s.thread_id for s in profile.samples))} threads")
profile.save("threaded_profile.json")
```

---

## Kubernetes / Container Usage

spprof works in restricted container environments:

```yaml
# kubernetes deployment
apiVersion: apps/v1
kind: Deployment
spec:
  template:
    spec:
      securityContext:
        runAsNonRoot: true
        readOnlyRootFilesystem: true
      containers:
        - name: app
          volumeMounts:
            - name: tmp
              mountPath: /tmp
          # No special capabilities needed!
      volumes:
        - name: tmp
          emptyDir: {}
```

```python
# In your application
import spprof

# Write to writable temp volume
spprof.start(interval_ms=10, output_path="/tmp/profile.json")
# ... application runs ...
spprof.stop()  # Profile saved to /tmp/profile.json
```

---

## Configuration Options

| Parameter | Default | Range | Description |
|-----------|---------|-------|-------------|
| `interval_ms` | 10 | 1–1000 | Sampling interval in milliseconds |
| `output_path` | None | - | Auto-save path (optional) |

### Choosing Sample Interval

| Interval | Overhead | Use Case |
|----------|----------|----------|
| 1 ms | ~3-5% | Short-running scripts, high precision |
| 10 ms | < 1% | Production profiling (default) |
| 100 ms | < 0.1% | Long-running monitoring |

---

## Troubleshooting

### "Profiler already running"

```python
# Check status before starting
if not spprof.is_active():
    spprof.start()
```

### Empty profile (no samples)

- Check that workload uses CPU (I/O-bound code may not generate samples)
- Ensure interval is not longer than workload duration
- Verify threads are executing Python code (not blocked in C)

### Permission errors

```python
# Use writable path
spprof.start(output_path="/tmp/profile.json")  # Not /app/profile.json
```

### Signal conflicts

If your application uses `SIGPROF`:

```python
import signal

# Save existing handler
old_handler = signal.getsignal(signal.SIGPROF)

spprof.start()
# ... profile ...
spprof.stop()

# Restore handler
signal.signal(signal.SIGPROF, old_handler)
```

---

## API Reference

See [contracts/python-api.md](contracts/python-api.md) for complete API documentation.

---

## Performance Characteristics

| Metric | Value |
|--------|-------|
| Startup time | < 10 ms |
| Shutdown time | < 100 ms |
| Memory overhead | ~16 MB (ring buffer) + ~32 MB (symbol cache) |
| CPU overhead @ 10ms | < 1% |
| CPU overhead @ 1ms | < 5% |

