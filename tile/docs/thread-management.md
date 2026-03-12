# Thread Management

spprof supports multi-threaded profiling. Platform behavior differs:

- **Linux**: Uses per-thread CPU timers (`timer_create` + SIGPROF). Each thread that should be profiled must explicitly register a timer. Worker threads created after `start()` must call `register_thread()` themselves.
- **macOS**: Uses Mach thread suspension — all threads are sampled automatically. `register_thread()` and `unregister_thread()` are no-ops.
- **Windows**: Uses timer queue — all threads are sampled automatically. Registration functions are no-ops.

## Capabilities

### Explicit Registration

Register and unregister threads manually. Recommended on Linux for worker threads.

```python { .api }
def register_thread() -> bool:
    """
    Register the current thread for profiling.

    On Linux: installs a per-thread CPU timer (SIGPROF). Call from any thread
    that should be profiled alongside the main thread. The main thread is
    registered automatically by start().

    On macOS / Windows: no-op (returns True immediately).

    Returns:
    - bool: True if registration succeeded or was not needed.
    """
```

```python { .api }
def unregister_thread() -> bool:
    """
    Unregister the current thread from profiling.

    Cleans up per-thread timer resources. Optional but recommended for
    long-running applications that create and destroy many threads.

    On macOS / Windows: no-op (returns True immediately).

    Returns:
    - bool: True if unregistration succeeded or was not needed.
    """
```

Usage example:

```python
import threading
import spprof

def worker():
    spprof.register_thread()
    try:
        # ... do work that should be profiled ...
        result = compute_something()
    finally:
        spprof.unregister_thread()
    return result

spprof.start()
t = threading.Thread(target=worker)
t.start()
t.join()
profile = spprof.stop()
```

### ThreadProfiler Context Manager

Automatically handles thread registration and cleanup using a `with` block.

```python { .api }
class ThreadProfiler:
    """
    Context manager for thread-local profiling setup.
    Calls register_thread() on __enter__ and unregister_thread() on __exit__.
    """

    def __enter__(self) -> ThreadProfiler: ...

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc_val: BaseException | None,
        exc_tb: Any,
    ) -> None: ...
```

Usage example:

```python
import threading
import spprof

def worker():
    with spprof.ThreadProfiler():
        # This thread is now registered and will be sampled
        compute_something()
    # Thread is unregistered after the block

spprof.start()
threads = [threading.Thread(target=worker) for _ in range(4)]
for t in threads:
    t.start()
for t in threads:
    t.join()
profile = spprof.stop()
profile.save("multi_thread_profile.json")
```

## Platform Details

| Platform | Mechanism | Thread Sampling | Free-Threading |
|----------|-----------|-----------------|----------------|
| Linux | `timer_create` + SIGPROF | Per-thread CPU time, explicit registration | Supported (Python 3.13+) |
| macOS | Mach thread suspension | All threads automatically | Supported |
| Windows | Timer queue + GIL | All threads automatically | Not supported |

**Linux free-threaded Python (3.13+)**: Supported via speculative capture with validation. Approximately 0.0005% sample drop rate.

**Container environments**: In restricted containers (seccomp/cgroups v1), spprof falls back to wall-time sampling automatically. For full CPU-time profiling on Docker, use `--security-opt seccomp=unconfined` (development only).
