"""
spprof - High-performance sampling profiler for Python applications.

Example usage:
    >>> import spprof
    >>> spprof.start(interval_ms=10)
    >>> # ... run workload ...
    >>> profile = spprof.stop()
    >>> profile.save("profile.json")
"""

from __future__ import annotations

import functools
import platform
import sys
import threading
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import TYPE_CHECKING, Any, Callable, Literal, TypeVar


if TYPE_CHECKING:
    from collections.abc import Sequence


__version__ = "0.1.0"

# Try to import native extension, fallback to pure Python
_HAS_NATIVE = False
_native: Any = None


def _try_import_native() -> None:
    """Try to import the native C extension."""
    global _HAS_NATIVE, _native
    try:
        import spprof._native as _native_module

        if hasattr(_native_module, "_start") and hasattr(_native_module, "_stop"):
            _native = _native_module
            _HAS_NATIVE = True
    except ImportError:
        pass
    except Exception:
        pass


_try_import_native()


# --- Data Classes ---


@dataclass(frozen=True)
class Frame:
    """A single frame in a call stack."""

    function_name: str
    filename: str
    lineno: int
    is_native: bool = False


@dataclass(frozen=True)
class Sample:
    """A single profiling sample."""

    timestamp_ns: int
    thread_id: int
    thread_name: str | None
    frames: Sequence[Frame]  # Bottom to top


@dataclass
class ProfilerStats:
    """Statistics from a profiling session."""

    collected_samples: int
    dropped_samples: int
    duration_ms: float
    overhead_estimate_pct: float


@dataclass
class Profile:
    """Result of a profiling session."""

    start_time: datetime
    end_time: datetime
    interval_ms: int
    samples: list[Sample]
    dropped_count: int
    python_version: str
    platform: str

    @property
    def sample_count(self) -> int:
        """Number of samples collected."""
        return len(self.samples)

    @property
    def total_duration_ms(self) -> float:
        """Total profiling duration in milliseconds."""
        delta = self.end_time - self.start_time
        return delta.total_seconds() * 1000

    @property
    def effective_rate_hz(self) -> float:
        """Effective sampling rate in Hz (samples per second)."""
        duration_s = self.total_duration_ms / 1000
        if duration_s <= 0:
            return 0.0
        return self.sample_count / duration_s

    def to_speedscope(self) -> dict[str, Any]:
        """Convert to Speedscope JSON format."""
        from spprof.output import to_speedscope

        return to_speedscope(self)

    def to_collapsed(self) -> str:
        """Convert to collapsed stack format (for FlameGraph)."""
        from spprof.output import to_collapsed

        return to_collapsed(self)

    def save(
        self, path: Path | str, format: Literal["speedscope", "collapsed"] = "speedscope"
    ) -> None:
        """Save profile to file."""
        import json

        output_path = Path(path)

        if format == "speedscope":
            speedscope_data = self.to_speedscope()
            output_path.write_text(json.dumps(speedscope_data, indent=2))
        elif format == "collapsed":
            collapsed_data = self.to_collapsed()
            output_path.write_text(collapsed_data)
        else:
            raise ValueError(f"Unknown format: {format}")


# --- Global State (for pure Python fallback) ---

_profiler_lock = threading.Lock()
_is_active = False
_start_time: datetime | None = None
_interval_ms: int = 10
_samples: list[Sample] = []
_output_path: Path | str | None = None


# --- Core API ---


def start(
    interval_ms: int = 10,
    output_path: Path | str | None = None,
    memory_limit_mb: int = 100,
) -> None:
    """
    Start CPU profiling.

    Args:
        interval_ms: Sampling interval in milliseconds. Default 10ms.
                    Minimum 1ms. Lower values = higher overhead.
        output_path: Optional path to write profile on stop().
                    If None, profile returned from stop().
        memory_limit_mb: Maximum memory usage in MB. Default 100MB.

    Raises:
        RuntimeError: If profiling is already active.
        ValueError: If interval_ms < 1.
        PermissionError: If output_path is not writable.

    Example:
        >>> import spprof
        >>> spprof.start(interval_ms=10)
        >>> # ... run workload ...
        >>> profile = spprof.stop()
    """
    global _is_active, _start_time, _interval_ms, _samples, _output_path

    if interval_ms < 1:
        raise ValueError("interval_ms must be >= 1")

    with _profiler_lock:
        if _is_active:
            raise RuntimeError("Profiler already running")

        if output_path is not None:
            # Verify path is writable
            output_path = Path(output_path)
            try:
                output_path.parent.mkdir(parents=True, exist_ok=True)
                # Test write
                output_path.touch()
            except (OSError, PermissionError) as e:
                raise PermissionError(f"Cannot write to {output_path}: {e}") from e

        _output_path = output_path
        _interval_ms = interval_ms
        _samples = []
        _start_time = datetime.now()

        if _HAS_NATIVE:
            interval_ns = interval_ms * 1_000_000
            _native._start(interval_ns=interval_ns)

        _is_active = True


def stop() -> Profile:
    """
    Stop CPU profiling and return results.

    Returns:
        Profile object containing all collected samples.

    Raises:
        RuntimeError: If profiling is not active.

    Example:
        >>> profile = spprof.stop()
        >>> print(f"Collected {len(profile.samples)} samples")
        >>> profile.save("profile.json")
    """
    global _is_active, _samples

    with _profiler_lock:
        if not _is_active:
            raise RuntimeError("Profiler not running")

        end_time = datetime.now()

        if _HAS_NATIVE:
            raw_samples = _native._stop()
            samples = _convert_raw_samples(raw_samples)
            dropped_count = 0  # TODO: Get from native
        else:
            samples = _samples
            dropped_count = 0

        profile = Profile(
            start_time=_start_time,  # type: ignore
            end_time=end_time,
            interval_ms=_interval_ms,
            samples=samples,
            dropped_count=dropped_count,
            python_version=f"{sys.version_info.major}.{sys.version_info.minor}.{sys.version_info.micro}",
            platform=f"{platform.system()}-{platform.release()}-{platform.machine()}",
        )

        _is_active = False
        _samples = []

        # Auto-save if output path was specified
        if _output_path is not None:
            profile.save(_output_path)

        return profile


def is_active() -> bool:
    """
    Check if profiling is currently active.

    Returns:
        True if profiler is running, False otherwise.
    """
    if _HAS_NATIVE:
        return bool(_native._is_active())
    return _is_active


def stats() -> ProfilerStats | None:
    """
    Get current profiling statistics.

    Returns:
        ProfilerStats if profiling is active, None otherwise.
    """
    if not is_active():
        return None

    if _HAS_NATIVE:
        raw_stats = _native._get_stats()
        return ProfilerStats(
            collected_samples=raw_stats.get("collected_samples", 0),
            dropped_samples=raw_stats.get("dropped_samples", 0),
            duration_ms=raw_stats.get("duration_ns", 0) / 1_000_000,
            overhead_estimate_pct=0.0,  # TODO: Calculate
        )

    return ProfilerStats(
        collected_samples=len(_samples),
        dropped_samples=0,
        duration_ms=(datetime.now() - _start_time).total_seconds() * 1000 if _start_time else 0,
        overhead_estimate_pct=0.0,
    )


# --- Thread Management API ---


def register_thread() -> bool:
    """
    Register the current thread for profiling.

    On Linux, each thread needs its own profiling timer for accurate sampling.
    Call this from any thread that should be profiled alongside the main thread.

    On macOS and Windows, this is a no-op as those platforms sample all threads
    automatically.

    Returns:
        True if registration succeeded or was not needed.

    Example:
        >>> import threading
        >>> import spprof
        >>>
        >>> def worker():
        ...     spprof.register_thread()
        ...     # ... do work ...
        >>>
        >>> spprof.start()
        >>> t = threading.Thread(target=worker)
        >>> t.start()
    """
    if not _HAS_NATIVE:
        return True  # Pure Python doesn't need registration

    if hasattr(_native, "_register_thread"):
        try:
            return bool(_native._register_thread())
        except Exception:
            return False
    return True


def unregister_thread() -> bool:
    """
    Unregister the current thread from profiling.

    Call this before a profiled thread exits to clean up resources.
    This is optional on most platforms but recommended for long-running
    applications that create/destroy many threads.

    Returns:
        True if unregistration succeeded or was not needed.
    """
    if not _HAS_NATIVE:
        return True

    if hasattr(_native, "_unregister_thread"):
        try:
            return bool(_native._unregister_thread())
        except Exception:
            return False
    return True


class ThreadProfiler:
    """
    Context manager for thread-local profiling setup.

    Automatically registers and unregisters the thread for profiling.

    Example:
        >>> import threading
        >>> import spprof
        >>>
        >>> def worker():
        ...     with spprof.ThreadProfiler():
        ...         # This thread is now being sampled
        ...         compute_something()
        >>>
        >>> spprof.start()
        >>> t = threading.Thread(target=worker)
        >>> t.start()
    """

    def __enter__(self) -> ThreadProfiler:
        register_thread()
        return self

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc_val: BaseException | None,
        exc_tb: Any,
    ) -> None:
        unregister_thread()


# --- Native Unwinding API ---


@dataclass(frozen=True)
class NativeFrame:
    """A native (C/C++) stack frame."""

    ip: int  # Instruction pointer
    symbol: str  # Symbol name
    filename: str  # Object file
    offset: int  # Offset from symbol start
    resolved: bool  # True if symbol was resolved


def native_unwinding_available() -> bool:
    """
    Check if native C-stack unwinding is available on this platform.

    Native unwinding allows capturing C/C++ frames alongside Python frames,
    enabling mixed-mode profiling for debugging C extensions.

    Returns:
        True if available (Linux with libunwind, macOS with backtrace).
    """
    if _HAS_NATIVE and hasattr(_native, "_native_unwinding_available"):
        return bool(_native._native_unwinding_available())
    return False


def set_native_unwinding(enabled: bool) -> None:
    """
    Enable or disable native C-stack unwinding.

    When enabled, the profiler will also capture C/C++ stack frames
    using libunwind (Linux) or backtrace (macOS). This enables
    mixed-mode profiling but adds overhead.

    Args:
        enabled: True to enable, False to disable.

    Raises:
        RuntimeError: If native unwinding is not available.

    Example:
        >>> import spprof
        >>> if spprof.native_unwinding_available():
        ...     spprof.set_native_unwinding(True)
        >>> spprof.start()
    """
    if not _HAS_NATIVE:
        if enabled:
            raise RuntimeError("Native unwinding requires the C extension")
        return

    if hasattr(_native, "_set_native_unwinding"):
        _native._set_native_unwinding(enabled)
    elif enabled:
        raise RuntimeError("Native unwinding not available")


def native_unwinding_enabled() -> bool:
    """
    Check if native C-stack unwinding is currently enabled.

    Returns:
        True if enabled, False otherwise.
    """
    if _HAS_NATIVE and hasattr(_native, "_native_unwinding_enabled"):
        return bool(_native._native_unwinding_enabled())
    return False


def capture_native_stack() -> list[NativeFrame]:
    """
    Capture the current native (C/C++) call stack.

    This is primarily for debugging and testing. It captures the
    current C-level stack trace and resolves symbols where possible.

    Returns:
        List of NativeFrame objects representing the call stack.

    Raises:
        RuntimeError: If native unwinding is not available.

    Example:
        >>> import spprof
        >>> frames = spprof.capture_native_stack()
        >>> for f in frames:
        ...     print(f"{f.symbol} ({f.filename})")
    """
    if not _HAS_NATIVE:
        raise RuntimeError("Native unwinding requires the C extension")

    if not hasattr(_native, "_capture_native_stack"):
        raise RuntimeError("Native unwinding not available")

    raw_frames = _native._capture_native_stack()
    return [
        NativeFrame(
            ip=f.get("ip", 0),
            symbol=f.get("symbol", ""),
            filename=f.get("filename", ""),
            offset=f.get("offset", 0),
            resolved=f.get("resolved", False),
        )
        for f in raw_frames
    ]


# --- Context Manager API ---


class Profiler:
    """
    Context manager for profiling a code block.

    Example:
        >>> with spprof.Profiler(interval_ms=5) as p:
        ...     # ... run workload ...
        >>> p.profile.save("profile.json")
    """

    def __init__(
        self,
        interval_ms: int = 10,
        output_path: Path | str | None = None,
        memory_limit_mb: int = 100,
    ) -> None:
        self._interval_ms = interval_ms
        self._output_path = output_path
        self._memory_limit_mb = memory_limit_mb
        self._profile: Profile | None = None

    def __enter__(self) -> Profiler:
        start(
            interval_ms=self._interval_ms,
            output_path=None,  # Don't auto-save; let user call save()
            memory_limit_mb=self._memory_limit_mb,
        )
        return self

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc_val: BaseException | None,
        exc_tb: Any,
    ) -> None:
        self._profile = stop()
        if self._output_path is not None:
            self._profile.save(self._output_path)

    @property
    def profile(self) -> Profile | None:
        """Profile result after exiting context. None while profiling."""
        return self._profile


# --- Decorator API ---

F = TypeVar("F", bound=Callable[..., Any])


def profile(
    interval_ms: int = 10,
    output_path: Path | str | None = None,
) -> Callable[[F], F]:
    """
    Decorator to profile a function.

    Example:
        >>> @spprof.profile(output_path="compute.json")
        ... def heavy_computation():
        ...     # ... expensive work ...
    """

    def decorator(func: F) -> F:
        @functools.wraps(func)
        def wrapper(*args: Any, **kwargs: Any) -> Any:
            with Profiler(interval_ms=interval_ms, output_path=output_path):
                return func(*args, **kwargs)

        return wrapper  # type: ignore

    return decorator


# --- Helper Functions ---


def _convert_raw_samples(raw_samples: list[dict[str, Any]]) -> list[Sample]:
    """Convert raw samples from native extension to Sample objects."""
    samples = []
    thread_names = _get_thread_names()

    for raw in raw_samples:
        frames = [
            Frame(
                function_name=f.get("function", "<unknown>"),
                filename=f.get("filename", "<unknown>"),
                lineno=f.get("lineno", 0),
                is_native=f.get("is_native", False),
            )
            for f in raw.get("frames", [])
        ]

        thread_id = raw.get("thread_id", 0)
        sample = Sample(
            timestamp_ns=raw.get("timestamp", 0),
            thread_id=thread_id,
            thread_name=thread_names.get(thread_id),
            frames=frames,
        )
        samples.append(sample)

    return samples


def _get_thread_names() -> dict[int, str]:
    """Get mapping of thread IDs to names."""
    names = {}
    for thread in threading.enumerate():
        if hasattr(thread, "ident") and thread.ident is not None:
            names[thread.ident] = thread.name
    return names


# --- Public API ---

__all__ = [
    # Data classes
    "Frame",
    "NativeFrame",
    "Profile",
    # Context manager
    "Profiler",
    "ProfilerStats",
    "Sample",
    "ThreadProfiler",
    "__version__",
    "capture_native_stack",
    "is_active",
    # Native unwinding
    "native_unwinding_available",
    "native_unwinding_enabled",
    # Decorator
    "profile",
    # Thread management
    "register_thread",
    "set_native_unwinding",
    # Core API
    "start",
    "stats",
    "stop",
    "unregister_thread",
]
