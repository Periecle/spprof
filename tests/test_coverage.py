"""Additional tests for improving code coverage."""

import tempfile
from datetime import datetime
from pathlib import Path

import pytest


def test_profile_properties():
    """Test Profile dataclass properties."""
    from spprof import Frame, Profile, Sample

    # Create a profile with samples
    samples = [
        Sample(
            timestamp_ns=1000000,
            thread_id=1,
            thread_name="MainThread",
            frames=[Frame("func1", "file.py", 10)],
        ),
        Sample(
            timestamp_ns=2000000,
            thread_id=1,
            thread_name="MainThread",
            frames=[Frame("func2", "file.py", 20)],
        ),
    ]

    profile = Profile(
        start_time=datetime(2024, 1, 1, 12, 0, 0),
        end_time=datetime(2024, 1, 1, 12, 0, 1),  # 1 second later
        interval_ms=10,
        samples=samples,
        dropped_count=0,
        python_version="3.12.0",
        platform="linux",
    )

    # Test sample_count
    assert profile.sample_count == 2

    # Test total_duration_ms
    assert profile.total_duration_ms == 1000.0  # 1 second = 1000ms

    # Test effective_rate_hz
    assert profile.effective_rate_hz == 2.0  # 2 samples in 1 second


def test_profile_effective_rate_zero_duration():
    """Test effective_rate_hz with zero duration."""
    from spprof import Profile

    profile = Profile(
        start_time=datetime(2024, 1, 1, 12, 0, 0),
        end_time=datetime(2024, 1, 1, 12, 0, 0),  # Same time
        interval_ms=10,
        samples=[],
        dropped_count=0,
        python_version="3.12.0",
        platform="linux",
    )

    # Should return 0.0 for zero duration
    assert profile.effective_rate_hz == 0.0


def test_profile_save_invalid_format():
    """Test Profile.save() with invalid format raises ValueError."""
    from spprof import Profile

    profile = Profile(
        start_time=datetime(2024, 1, 1, 12, 0, 0),
        end_time=datetime(2024, 1, 1, 12, 0, 1),
        interval_ms=10,
        samples=[],
        dropped_count=0,
        python_version="3.12.0",
        platform="linux",
    )

    with (
        tempfile.NamedTemporaryFile(suffix=".txt", delete=False) as f,
        pytest.raises(ValueError, match="Unknown format"),
    ):
        profile.save(f.name, format="invalid_format")  # type: ignore


def test_collapsed_format_edge_cases():
    """Test collapsed format with edge cases."""
    from spprof import Frame, Profile, Sample

    # Test with empty frames
    profile_empty = Profile(
        start_time=datetime(2024, 1, 1, 12, 0, 0),
        end_time=datetime(2024, 1, 1, 12, 0, 1),
        interval_ms=10,
        samples=[
            Sample(timestamp_ns=1000, thread_id=1, thread_name=None, frames=[]),
        ],
        dropped_count=0,
        python_version="3.12.0",
        platform="linux",
    )

    collapsed = profile_empty.to_collapsed()
    assert collapsed == ""  # Empty frames should produce empty output

    # Test with frame without filename/lineno
    profile_no_file = Profile(
        start_time=datetime(2024, 1, 1, 12, 0, 0),
        end_time=datetime(2024, 1, 1, 12, 0, 1),
        interval_ms=10,
        samples=[
            Sample(
                timestamp_ns=1000,
                thread_id=1,
                thread_name=None,
                frames=[Frame("native_func", "", 0)],  # No filename, lineno=0
            ),
        ],
        dropped_count=0,
        python_version="3.12.0",
        platform="linux",
    )

    collapsed = profile_no_file.to_collapsed()
    assert "native_func" in collapsed
    # Should not have file:line format when missing
    assert "(:" not in collapsed or "native_func 1" in collapsed


def test_speedscope_empty_thread_samples():
    """Test speedscope format handles edge cases."""
    from spprof import Profile

    # Profile with no samples
    profile = Profile(
        start_time=datetime(2024, 1, 1, 12, 0, 0),
        end_time=datetime(2024, 1, 1, 12, 0, 1),
        interval_ms=10,
        samples=[],
        dropped_count=0,
        python_version="3.12.0",
        platform="linux",
    )

    speedscope = profile.to_speedscope()
    assert speedscope["profiles"] == []
    assert speedscope["shared"]["frames"] == []


def test_output_version_fallback():
    """Test _get_version function."""
    from spprof.output import _get_version

    version = _get_version()
    assert version  # Should return something
    assert "." in version  # Should be version format like "0.1.0"


def test_stats_when_inactive():
    """Test stats() returns None when profiler is inactive."""
    import spprof

    # Make sure profiler is not active
    if spprof.is_active():
        spprof.stop()

    result = spprof.stats()
    assert result is None


def test_profiler_with_output_path():
    """Test profiler with output_path parameter."""
    import spprof

    with tempfile.NamedTemporaryFile(suffix=".json", delete=False) as f:
        output_path = f.name

    try:
        spprof.start(interval_ms=10, output_path=output_path)

        # Do some work
        total = 0
        for i in range(10000):
            total += i

        profile = spprof.stop()

        # File should be created
        assert Path(output_path).exists()
        assert profile is not None
    finally:
        # Cleanup
        Path(output_path).unlink(missing_ok=True)


def test_native_unwinding_not_available():
    """Test native unwinding functions when not available."""
    import spprof

    # These should return False/empty without errors
    available = spprof.native_unwinding_available()
    assert isinstance(available, bool)

    enabled = spprof.native_unwinding_enabled()
    assert isinstance(enabled, bool)


def test_thread_registration_when_not_active():
    """Test thread registration when profiler is not active."""
    import spprof

    # Should succeed as no-op
    result = spprof.register_thread()
    assert result is True

    result = spprof.unregister_thread()
    assert result is True


def test_thread_profiler_context_manager_basic():
    """Test ThreadProfiler context manager."""
    import spprof

    spprof.start(interval_ms=10)

    with spprof.ThreadProfiler():
        # Just enter and exit
        pass

    spprof.stop()


def test_profile_decorator_basic():
    """Test profile decorator."""
    import spprof

    @spprof.profile(interval_ms=10)
    def simple_func():
        total = 0
        for i in range(1000):
            total += i
        return total

    result = simple_func()
    assert result == sum(range(1000))


def test_frame_is_native_default():
    """Test Frame is_native default value."""
    from spprof import Frame

    frame = Frame("func", "file.py", 10)
    assert frame.is_native is False

    frame_native = Frame("native", "lib.so", 0, is_native=True)
    assert frame_native.is_native is True


def test_sample_with_none_thread_name():
    """Test Sample with None thread name."""
    from spprof import Frame, Sample

    sample = Sample(
        timestamp_ns=1000,
        thread_id=123,
        thread_name=None,
        frames=[Frame("func", "file.py", 10)],
    )

    assert sample.thread_name is None
    assert sample.thread_id == 123


def test_profiler_stats_structure():
    """Test ProfilerStats structure."""
    from spprof import ProfilerStats

    stats = ProfilerStats(
        collected_samples=100,
        dropped_samples=5,
        duration_ms=1000.0,
        overhead_estimate_pct=0.5,
    )

    assert stats.collected_samples == 100
    assert stats.dropped_samples == 5
    assert stats.duration_ms == 1000.0
    assert stats.overhead_estimate_pct == 0.5


def test_speedscope_frame_cache():
    """Test speedscope frame deduplication."""
    from spprof import Frame, Profile, Sample

    # Same frame appearing multiple times should be deduplicated
    samples = [
        Sample(
            timestamp_ns=1000,
            thread_id=1,
            thread_name="Main",
            frames=[
                Frame("func", "file.py", 10),
                Frame("func", "file.py", 10),  # Duplicate
            ],
        ),
        Sample(
            timestamp_ns=2000,
            thread_id=1,
            thread_name="Main",
            frames=[Frame("func", "file.py", 10)],  # Same frame again
        ),
    ]

    profile = Profile(
        start_time=datetime(2024, 1, 1, 12, 0, 0),
        end_time=datetime(2024, 1, 1, 12, 0, 1),
        interval_ms=10,
        samples=samples,
        dropped_count=0,
        python_version="3.12.0",
        platform="linux",
    )

    speedscope = profile.to_speedscope()
    # Should only have 1 unique frame
    assert len(speedscope["shared"]["frames"]) == 1


def test_speedscope_multiple_threads():
    """Test speedscope format with multiple threads."""
    from spprof import Frame, Profile, Sample

    samples = [
        Sample(
            timestamp_ns=1000,
            thread_id=1,
            thread_name="Main",
            frames=[Frame("main_func", "main.py", 10)],
        ),
        Sample(
            timestamp_ns=1000,
            thread_id=2,
            thread_name="Worker",
            frames=[Frame("worker_func", "worker.py", 20)],
        ),
        Sample(
            timestamp_ns=2000,
            thread_id=1,
            thread_name="Main",
            frames=[Frame("main_func", "main.py", 10)],
        ),
    ]

    profile = Profile(
        start_time=datetime(2024, 1, 1, 12, 0, 0),
        end_time=datetime(2024, 1, 1, 12, 0, 1),
        interval_ms=10,
        samples=samples,
        dropped_count=0,
        python_version="3.12.0",
        platform="linux",
    )

    speedscope = profile.to_speedscope()
    # Should have 2 profiles (one per thread)
    assert len(speedscope["profiles"]) == 2


def test_speedscope_thread_without_name():
    """Test speedscope format with thread without name."""
    from spprof import Frame, Profile, Sample

    samples = [
        Sample(
            timestamp_ns=1000,
            thread_id=12345,
            thread_name=None,  # No name
            frames=[Frame("func", "file.py", 10)],
        ),
    ]

    profile = Profile(
        start_time=datetime(2024, 1, 1, 12, 0, 0),
        end_time=datetime(2024, 1, 1, 12, 0, 1),
        interval_ms=10,
        samples=samples,
        dropped_count=0,
        python_version="3.12.0",
        platform="linux",
    )

    speedscope = profile.to_speedscope()
    # Should use "Thread-{id}" as fallback name
    assert "Thread-12345" in speedscope["profiles"][0]["name"]


def test_set_native_unwinding_when_not_available():
    """Test set_native_unwinding when native extension not available."""
    import spprof

    # If native unwinding is not available, setting it should raise
    if not spprof.native_unwinding_available():
        with pytest.raises(RuntimeError):
            spprof.set_native_unwinding(True)

        # Disabling should be a no-op (no error)
        spprof.set_native_unwinding(False)


def test_capture_native_stack_requires_native():
    """Test capture_native_stack requires native extension."""
    import spprof

    if not spprof.native_unwinding_available():
        with pytest.raises(RuntimeError):
            spprof.capture_native_stack()


def test_collapsed_with_native_frames():
    """Test collapsed format with native frames."""
    from spprof import Frame, Profile, Sample

    samples = [
        Sample(
            timestamp_ns=1000,
            thread_id=1,
            thread_name="Main",
            frames=[
                Frame("native_func", "libc.so", 0, is_native=True),
                Frame("python_func", "script.py", 42, is_native=False),
            ],
        ),
    ]

    profile = Profile(
        start_time=datetime(2024, 1, 1, 12, 0, 0),
        end_time=datetime(2024, 1, 1, 12, 0, 1),
        interval_ms=10,
        samples=samples,
        dropped_count=0,
        python_version="3.12.0",
        platform="linux",
    )

    collapsed = profile.to_collapsed()
    assert "native_func" in collapsed
    assert "python_func" in collapsed


def test_profiler_context_with_output_path():
    """Test Profiler context manager with output_path."""
    import spprof

    with tempfile.NamedTemporaryFile(suffix=".json", delete=False) as f:
        output_path = f.name

    try:
        with spprof.Profiler(interval_ms=10, output_path=output_path) as p:
            total = 0
            for i in range(1000):
                total += i

        # Profile should be saved
        assert Path(output_path).exists()
        assert p.profile is not None
    finally:
        Path(output_path).unlink(missing_ok=True)


def test_profile_property_before_exit():
    """Test profile property returns None before exit."""
    import spprof

    profiler = spprof.Profiler(interval_ms=10)
    # Before entering context, profile should be None
    assert profiler.profile is None


def test_start_with_unwritable_path():
    """Test start() with unwritable output path raises PermissionError."""
    import spprof

    # Try to write to a path that doesn't exist and can't be created
    with pytest.raises(PermissionError):
        spprof.start(interval_ms=10, output_path="/nonexistent/deep/path/file.json")


def test_native_frame_repr():
    """Test NativeFrame dataclass."""
    from spprof import NativeFrame

    frame = NativeFrame(
        ip=0x12345678,
        symbol="test_func",
        filename="libtest.so",
        offset=0x100,
        resolved=True,
    )

    assert frame.ip == 0x12345678
    assert frame.symbol == "test_func"
    assert frame.filename == "libtest.so"
    assert frame.offset == 0x100
    assert frame.resolved is True


def test_collapsed_multiple_same_stacks():
    """Test collapsed format counts duplicate stacks."""
    from spprof import Frame, Profile, Sample

    # Same stack appearing 3 times
    frames = [Frame("func", "file.py", 10)]
    samples = [
        Sample(timestamp_ns=1000, thread_id=1, thread_name="Main", frames=frames),
        Sample(timestamp_ns=2000, thread_id=1, thread_name="Main", frames=frames),
        Sample(timestamp_ns=3000, thread_id=1, thread_name="Main", frames=frames),
    ]

    profile = Profile(
        start_time=datetime(2024, 1, 1, 12, 0, 0),
        end_time=datetime(2024, 1, 1, 12, 0, 1),
        interval_ms=10,
        samples=samples,
        dropped_count=0,
        python_version="3.12.0",
        platform="linux",
    )

    collapsed = profile.to_collapsed()
    # Should show count of 3
    assert "3" in collapsed


def test_save_collapsed_format():
    """Test saving profile in collapsed format."""
    import spprof

    with tempfile.NamedTemporaryFile(suffix=".collapsed", delete=False) as f:
        output_path = f.name

    try:
        spprof.start(interval_ms=10)
        total = 0
        for i in range(10000):
            total += i
        profile = spprof.stop()

        profile.save(output_path, format="collapsed")

        # Verify file was created and has content
        assert Path(output_path).exists()
        content = Path(output_path).read_text()
        # May be empty if no samples, but file should exist
        assert isinstance(content, str)
    finally:
        Path(output_path).unlink(missing_ok=True)
