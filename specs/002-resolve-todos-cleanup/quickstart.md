# Quickstart: Resolve TODOs, Race Conditions, and Cleanup

This document provides validation steps for the changes in this feature.

## Prerequisites

- Python 3.9+ installed
- spprof built from source: `pip install -e .`
- Access to macOS for US2 testing (or Linux for US1/US3)

## User Story 1: Statistics API Validation

### Test Dropped Samples

Run a high-frequency profiling session to generate dropped samples:

```python
import spprof
import threading
import time

def cpu_intensive():
    """Generate CPU load to trigger frequent sampling."""
    x = 0
    for i in range(10_000_000):
        x = (x + i) % 12345

# Start with aggressive 1ms interval
spprof.start(interval_ms=1)

# Create many threads to overwhelm the ring buffer
threads = []
for _ in range(50):
    t = threading.Thread(target=cpu_intensive)
    threads.append(t)
    t.start()

for t in threads:
    t.join()

stats = spprof.stats()
print(f"Collected: {stats.collected_samples}")
print(f"Dropped: {stats.dropped_samples}")  # Should be > 0 under high load
print(f"Overhead: {stats.overhead_estimate_pct:.2f}%")

profile = spprof.stop()
print(f"Profile dropped_count: {profile.dropped_count}")
```

**Expected Results**:
- `stats.dropped_samples` reflects actual drops (may be 0 if buffer not full)
- `stats.overhead_estimate_pct` returns a positive value
- `profile.dropped_count` matches final dropped count

### Test Overhead Estimation

```python
import spprof
import time

spprof.start(interval_ms=10)

# Run for known duration
time.sleep(1.0)

stats = spprof.stats()
print(f"Duration: {stats.duration_ms:.2f} ms")
print(f"Samples: {stats.collected_samples}")
print(f"Estimated overhead: {stats.overhead_estimate_pct:.2f}%")

profile = spprof.stop()
```

**Expected Results**:
- Overhead estimate is reasonable (typically < 5% at 10ms interval)
- Overhead scales inversely with interval (1ms → higher overhead)

---

## User Story 2: macOS Race-Free Shutdown

### Test Rapid Start/Stop Cycles (macOS)

```python
import spprof
import sys

if sys.platform != 'darwin':
    print("This test is for macOS only")
    sys.exit(0)

print("Testing 1000 start/stop cycles...")
for i in range(1000):
    spprof.start(interval_ms=10)
    spprof.stop()
    if (i + 1) % 100 == 0:
        print(f"  Completed {i + 1} cycles")

print("SUCCESS: No crashes or hangs")
```

**Expected Results**:
- All 1000 cycles complete without crash
- No signal-related errors
- Completes in < 30 seconds

### Test with Concurrent Threads (macOS)

```python
import spprof
import threading
import time
import sys

if sys.platform != 'darwin':
    print("This test is for macOS only")
    sys.exit(0)

stop_flag = threading.Event()

def worker():
    while not stop_flag.is_set():
        x = sum(range(1000))
        time.sleep(0.001)

# Start background workers
threads = [threading.Thread(target=worker, daemon=True) for _ in range(10)]
for t in threads:
    t.start()

print("Testing start/stop with active threads...")
for i in range(100):
    spprof.start(interval_ms=5)
    time.sleep(0.01)  # Brief profiling window
    spprof.stop()
    if (i + 1) % 20 == 0:
        print(f"  Completed {i + 1} cycles")

stop_flag.set()
print("SUCCESS: No races detected")
```

**Expected Results**:
- No crashes during rapid start/stop
- Clean shutdown every time

---

## User Story 3: Repository Cleanup

### Verify No Backup Files

```bash
# From repository root
find src/ -name "*.backup" -type f

# Should return no results
```

### Verify Clean Git Status

```bash
git status

# Should show no untracked .backup files
```

---

## Full Test Suite

Run the complete test suite to ensure no regressions:

```bash
# Install in development mode
pip install -e .

# Run all tests
pytest tests/ -v

# Run platform-specific tests
pytest tests/test_platform.py -v
pytest tests/test_profiler.py -v
```

**Expected Results**:
- All tests pass
- No new warnings or errors

---

## Debugging Tips

### Enable Debug Output (if issues arise)

```bash
# Rebuild with debug symbols
CFLAGS="-DSPPROF_DEBUG -g" pip install -e . --force-reinstall

# Run with debug output
python your_script.py
```

### Check Signal Handling on macOS

```bash
# Trace signal operations
sudo dtruss -t sigaction python -c "import spprof; spprof.start(); import time; time.sleep(0.1); spprof.stop()"
```

---

## Success Checklist

- [ ] `spprof.stats().dropped_samples` returns actual value
- [ ] `spprof.stats().overhead_estimate_pct` returns positive value when profiling
- [ ] `profile.dropped_count` matches final stats
- [ ] macOS: 1000 start/stop cycles complete without crash
- [ ] macOS: No `nanosleep()` in darwin.c timer destroy
- [ ] No `.backup` files in `src/` directory
- [ ] All existing tests pass



