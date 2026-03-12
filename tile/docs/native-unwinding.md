# Native Unwinding (Mixed-Mode Profiling)

Native unwinding allows the profiler to capture C/C++ stack frames alongside Python frames, enabling mixed-mode profiling. This is useful for debugging C extensions or understanding performance bottlenecks in native code.

**Requirements**:
- Native C extension must be available (`_HAS_NATIVE = True`)
- Linux: requires `libunwind`
- macOS: uses `backtrace()` from the system library
- Windows: not supported

## Capabilities

### Check Availability

```python { .api }
def native_unwinding_available() -> bool:
    """
    Check if native C-stack unwinding is available on this platform.

    Returns:
    - bool: True if available (Linux with libunwind, macOS with backtrace).
             False if unavailable or if the C extension is not loaded.
    """
```

### Enable/Disable Native Unwinding

```python { .api }
def set_native_unwinding(enabled: bool) -> None:
    """
    Enable or disable native C-stack unwinding.

    When enabled, the profiler captures C/C++ stack frames using
    libunwind (Linux) or backtrace (macOS) in addition to Python frames.
    Results in NativeFrame entries appearing alongside Frame entries in samples.

    Parameters:
    - enabled (bool): True to enable, False to disable.

    Raises:
    - RuntimeError: If native unwinding is not available and enabled=True.
    - RuntimeError: If the C extension is not loaded and enabled=True.
    """
```

```python { .api }
def native_unwinding_enabled() -> bool:
    """
    Check if native C-stack unwinding is currently enabled.

    Returns:
    - bool: True if currently enabled, False otherwise.
    """
```

### Capture Current Native Stack

```python { .api }
def capture_native_stack() -> list[NativeFrame]:
    """
    Capture the current thread's native (C/C++) call stack.

    Primarily for debugging and testing. Captures the C-level stack trace
    at the point of the call and resolves symbols where possible.

    Returns:
    - list[NativeFrame]: List of native frames representing the call stack.

    Raises:
    - RuntimeError: If native unwinding is not available.
    - RuntimeError: If the C extension is not loaded.
    """
```

## NativeFrame Type

```python { .api }
from dataclasses import dataclass

@dataclass(frozen=True)
class NativeFrame:
    ip: int          # Instruction pointer address (memory address)
    symbol: str      # Symbol/function name (empty string if unresolved)
    filename: str    # Object file path (shared library or executable)
    offset: int      # Byte offset from the start of the symbol
    resolved: bool   # True if symbol name was successfully resolved
```

## Usage Example

```python
import spprof

# Check before enabling
if spprof.native_unwinding_available():
    spprof.set_native_unwinding(True)
    print("Mixed-mode profiling enabled")
else:
    print("Native unwinding not available, Python-only profiling")

spprof.start(interval_ms=10)
# ... run workload with C extensions ...
profile = spprof.stop()

# Inspect native frames in samples
for sample in profile.samples:
    native_frames = [f for f in sample.frames if f.is_native]
    if native_frames:
        print(f"Thread {sample.thread_id}: {len(native_frames)} native frames")

profile.save("mixed_profile.json")

# Capture current native stack for debugging
frames = spprof.capture_native_stack()
for frame in frames:
    status = "resolved" if frame.resolved else "unresolved"
    print(f"0x{frame.ip:x} {frame.symbol} @ {frame.filename}+{frame.offset} ({status})")
```

## Notes

- Native frames appear in `Sample.frames` with `is_native=True` on the `Frame` object.
- The `NativeFrame` type is only returned by `capture_native_stack()`, not by the profiler itself (native frames in profiling samples are represented as `Frame` objects with `is_native=True`).
- Enabling native unwinding adds overhead; not recommended for production unless specifically debugging C extensions.
- Toggle off with `set_native_unwinding(False)` before starting a production profile.
