# Quickstart: Linux timer_create Robustness Improvements

**Feature Branch**: `001-linux-create-timer`  
**Date**: 2025-12-01

---

## Prerequisites

### Development Environment

- **OS**: Linux (kernel 2.6+ for `timer_create` with `SIGEV_THREAD_ID`)
- **Compiler**: GCC 7+ or Clang 8+ with C11 support
- **Python**: 3.9-3.14 with development headers

### Required Packages (Ubuntu/Debian)

```bash
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    python3-dev \
    python3-pip \
    python3-venv \
    valgrind \
    linux-headers-$(uname -r)
```

### Required Packages (Fedora/RHEL)

```bash
sudo dnf install -y \
    gcc \
    make \
    python3-devel \
    valgrind \
    kernel-headers
```

---

## Setup

### 1. Clone and Checkout Branch

```bash
git clone https://github.com/Periecle/spprof.git
cd spprof
git checkout 001-linux-create-timer
```

### 2. Create Virtual Environment

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -e ".[dev]"
```

### 3. Verify Build

```bash
# Build the C extension
pip install -e .

# Verify installation
python -c "import spprof; print(spprof.__file__)"
```

---

## Development Workflow

### Building

```bash
# Full rebuild
pip install -e . --no-build-isolation --force-reinstall

# Quick rebuild (after C changes)
python setup.py build_ext --inplace
```

### Testing

```bash
# Run all tests
pytest tests/ -v

# Run Linux-specific tests only
pytest tests/test_platform.py -v -k "linux"

# Run threading tests
pytest tests/test_threading.py -v

# Run with verbose output
pytest tests/ -v --tb=long
```

### Memory Leak Detection

```bash
# Run tests under valgrind
valgrind --leak-check=full --suppressions=valgrind.supp \
    python -m pytest tests/test_platform.py -v

# Check for specific leaks in thread registry
valgrind --track-origins=yes \
    python -c "
import spprof
import threading

def work():
    sum(range(100000))

spprof.start(interval_ms=5)
threads = [threading.Thread(target=work) for _ in range(100)]
for t in threads: t.start()
for t in threads: t.join()
spprof.stop()
"
```

---

## Key Files to Modify

### Primary Changes

| File | Description |
|------|-------------|
| `src/spprof/_ext/platform/linux.c` | Main implementation - thread registry, pause/resume |
| `src/spprof/_ext/platform/platform.h` | Add pause/resume declarations |
| `tests/test_platform.py` | Add Linux robustness tests |
| `tests/test_threading.py` | Add thread limit tests |

### Supporting Files

| File | Description |
|------|-------------|
| `src/spprof/_ext/signal_handler.c` | Minor: expose additional stats |
| `src/spprof/_ext/signal_handler.h` | Minor: stat function declarations |

### New Files

| File | Description |
|------|-------------|
| `src/spprof/_ext/uthash.h` | Copy of uthash header (BSD license) |

---

## Adding uthash

Download the single header file:

```bash
curl -o src/spprof/_ext/uthash.h \
    https://raw.githubusercontent.com/troydhanson/uthash/master/src/uthash.h
```

Or add as a submodule:

```bash
git submodule add https://github.com/troydhanson/uthash.git vendor/uthash
ln -s ../../../vendor/uthash/src/uthash.h src/spprof/_ext/uthash.h
```

---

## Code Style

### C Code

- Follow existing style in `linux.c`
- Use `/* */` for comments (not `//`)
- Prefix static functions with module name: `registry_*`
- Use `_Atomic` for signal-safe variables
- Include thorough error handling

### Example

```c
/**
 * Add a thread to the registry.
 *
 * @param tid Thread ID
 * @param timer_id Timer handle
 * @return 0 on success, -1 on error
 */
static int registry_add_thread(pid_t tid, timer_t timer_id) {
    ThreadTimerEntry* entry = malloc(sizeof(ThreadTimerEntry));
    if (!entry) {
        return -1;
    }
    
    entry->tid = tid;
    entry->timer_id = timer_id;
    entry->overruns = 0;
    entry->active = 1;
    
    pthread_rwlock_wrlock(&g_registry_lock);
    HASH_ADD_INT(g_thread_registry, tid, entry);
    pthread_rwlock_unlock(&g_registry_lock);
    
    return 0;
}
```

---

## Testing Strategy

### Unit Tests

```python
# tests/test_linux_registry.py
import pytest
import platform

pytestmark = pytest.mark.skipif(
    platform.system() != "Linux",
    reason="Linux-specific tests"
)

def test_many_threads():
    """Test with more than 256 threads (old limit)."""
    import spprof
    import threading
    
    def work():
        sum(range(10000))
    
    spprof.start(interval_ms=10)
    
    # Create 300 threads (exceeds old limit)
    threads = [threading.Thread(target=work) for _ in range(300)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    
    profile = spprof.stop()
    assert profile is not None
    # Should have samples from all threads
```

### Stress Tests

```python
def test_rapid_thread_churn():
    """Test rapid thread creation/destruction."""
    import spprof
    import threading
    
    spprof.start(interval_ms=1)
    
    for i in range(100):
        threads = [threading.Thread(target=lambda: sum(range(1000))) 
                   for _ in range(50)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
    
    profile = spprof.stop()
    assert profile is not None
```

---

## Debugging Tips

### Enable Debug Output

```bash
# Build with debug flag
CFLAGS="-DSPPROF_DEBUG -g" pip install -e . --force-reinstall

# Run with debug output
python -c "import spprof; spprof.start(); spprof.stop()"
```

### GDB Debugging

```bash
# Start Python under GDB
gdb -ex run --args python -c "
import spprof
spprof.start(interval_ms=10)
import time; time.sleep(1)
spprof.stop()
"
```

### Check Timer Status

```bash
# View process timers
cat /proc/$(pgrep -f python)/timers

# Check signal delivery
strace -e signal python -c "import spprof; spprof.start(); import time; time.sleep(0.1); spprof.stop()"
```

---

## Common Issues

### Issue: `timer_create` fails with `EAGAIN`

**Cause**: System limit on number of timers reached.

**Solution**: Check and increase limits:
```bash
# Check current limit
cat /proc/sys/kernel/pid_max
ulimit -i  # Max pending signals

# Increase if needed (as root)
echo 65536 > /proc/sys/kernel/pid_max
```

### Issue: Segfault during shutdown

**Cause**: Race between timer deletion and signal delivery.

**Solution**: Ensure signals are blocked during cleanup (see research.md).

### Issue: Tests fail on non-Linux

**Expected**: This feature is Linux-only. Use `pytest.mark.skipif` for Linux-specific tests.

---

## Next Steps

After completing implementation:

1. Run full test suite: `pytest tests/ -v`
2. Run memory check: `valgrind --leak-check=full python -m pytest tests/`
3. Run stress tests: `pytest tests/test_threading.py -v -k stress`
4. Update documentation in `docs/ARCHITECTURE.md`
5. Create PR with benchmarks showing thread limit improvement

