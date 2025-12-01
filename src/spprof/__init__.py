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


@dataclass(frozen=True)
class StackTrace:
    """A unique call stack with frame tuple."""

    frames: tuple[Frame, ...]  # Immutable tuple for hashing
    thread_id: int
    thread_name: str | None = None

    def __hash__(self) -> int:
        return hash((self.frames, self.thread_id))


@dataclass
class AggregatedStack:
    """A call stack with its occurrence count."""

    frames: Sequence[Frame]
    thread_id: int
    thread_name: str | None
    count: int  # Number of times this exact stack was sampled


@dataclass
class AggregatedProfile:
    """Memory-efficient profile with aggregated identical stacks.

    For long-running profiles, many samples have identical stacks.
    This class stores unique stacks with counts instead of individual samples,
    reducing memory usage significantly.

    Example:
        >>> profile = spprof.stop()
        >>> agg = profile.aggregate()
        >>> print(f"Reduced {profile.sample_count} samples to {len(agg.stacks)} unique stacks")
        >>> agg.save("profile.json")  # Works with same output formats
    """

    start_time: datetime
    end_time: datetime
    interval_ms: int
    stacks: list[AggregatedStack]  # Unique stacks with counts
    total_samples: int  # Original sample count
    dropped_count: int
    python_version: str
    platform: str

    @property
    def unique_stack_count(self) -> int:
        """Number of unique stacks."""
        return len(self.stacks)

    @property
    def compression_ratio(self) -> float:
        """Ratio of original samples to unique stacks (higher = more compression)."""
        if self.unique_stack_count == 0:
            return 1.0
        return self.total_samples / self.unique_stack_count

    @property
    def memory_reduction_pct(self) -> float:
        """Estimated memory reduction percentage."""
        if self.total_samples == 0:
            return 0.0
        return (1.0 - (self.unique_stack_count / self.total_samples)) * 100

    def to_speedscope(self) -> dict[str, Any]:
        """Convert to Speedscope JSON format."""
        from spprof.output import aggregated_to_speedscope

        return aggregated_to_speedscope(self)

    def to_collapsed(self) -> str:
        """Convert to collapsed stack format (for FlameGraph)."""
        from spprof.output import aggregated_to_collapsed

        return aggregated_to_collapsed(self)

    def save(
        self, path: Path | str, format: Literal["speedscope", "collapsed"] = "speedscope"
    ) -> None:
        """Save aggregated profile to file."""
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

    def aggregate(self) -> AggregatedProfile:
        """Aggregate identical stacks to reduce memory usage.

        For long-running profiles, many samples have identical stacks.
        This method returns an AggregatedProfile that stores unique stacks
        with occurrence counts instead of individual samples.

        Returns:
            AggregatedProfile with unique stacks and counts.

        Example:
            >>> profile = spprof.stop()
            >>> agg = profile.aggregate()
            >>> print(f"Compression: {agg.compression_ratio:.1f}x")
            >>> print(f"Memory reduction: {agg.memory_reduction_pct:.1f}%")
        """
        from collections import Counter

        # Count unique stacks
        stack_counter: Counter[StackTrace] = Counter()

        for sample in self.samples:
            # Convert frames list to immutable tuple for hashing
            stack = StackTrace(
                frames=tuple(sample.frames),
                thread_id=sample.thread_id,
                thread_name=sample.thread_name,
            )
            stack_counter[stack] += 1

        # Convert to AggregatedStack list
        aggregated_stacks = [
            AggregatedStack(
                frames=list(stack.frames),
                thread_id=stack.thread_id,
                thread_name=stack.thread_name,
                count=count,
            )
            for stack, count in stack_counter.items()
        ]

        return AggregatedProfile(
            start_time=self.start_time,
            end_time=self.end_time,
            interval_ms=self.interval_ms,
            stacks=aggregated_stacks,
            total_samples=self.sample_count,
            dropped_count=self.dropped_count,
            python_version=self.python_version,
            platform=self.platform,
        )

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
            # Get final stats before stopping (includes dropped_samples)
            final_stats = _native._get_stats()
            dropped_count = final_stats.get("dropped_samples", 0) if final_stats else 0

            # Use streaming API to avoid OOM for long profiling sessions
            # Stop the timer first, then drain in chunks
            if hasattr(_native, "_stop_timer") and hasattr(_native, "_drain_buffer"):
                _native._stop_timer()

                # Drain samples in chunks to avoid memory spike
                all_raw_samples: list[dict[str, Any]] = []
                batch_size = 10000  # Process 10k samples at a time

                while True:
                    batch, has_more = _native._drain_buffer(batch_size)
                    all_raw_samples.extend(batch)
                    if not has_more:
                        break

                _native._finalize_stop()
                samples = _convert_raw_samples(all_raw_samples)
            else:
                # Fallback to legacy API for older native modules
                raw_samples = _native._stop()
                samples = _convert_raw_samples(raw_samples)
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
        collected = raw_stats.get("collected_samples", 0)
        duration_ms = raw_stats.get("duration_ns", 0) / 1_000_000

        # Estimate overhead: (samples * avg_handler_time_us) / duration
        # Using 25μs per sample as conservative estimate (includes frame walking,
        # ring buffer write, and signal dispatch overhead)
        avg_handler_time_ms = 0.025  # 25 microseconds in milliseconds
        if duration_ms > 0:
            overhead_estimate_pct = (collected * avg_handler_time_ms) / duration_ms * 100
        else:
            overhead_estimate_pct = 0.0

        return ProfilerStats(
            collected_samples=collected,
            dropped_samples=raw_stats.get("dropped_samples", 0),
            duration_ms=duration_ms,
            overhead_estimate_pct=overhead_estimate_pct,
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
    "AggregatedProfile",
    "AggregatedStack",
    "Frame",
    "NativeFrame",
    "Profile",
    # Context manager
    "Profiler",
    "ProfilerStats",
    "Sample",
    "StackTrace",
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
