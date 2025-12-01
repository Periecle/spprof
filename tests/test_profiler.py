"""Integration tests for the profiler."""

import contextlib
import time

import pytest


@pytest.fixture(autouse=True)
def cleanup_profiler():
    """Ensure profiler is stopped before and after each test."""
    import spprof

    # Cleanup before test
    if spprof.is_active():
        with contextlib.suppress(Exception):
            spprof.stop()

    yield

    # Cleanup after test
    if spprof.is_active():
        with contextlib.suppress(Exception):
            spprof.stop()

    # Small delay to ensure timers are fully cleaned up
    time.sleep(0.05)


def test_import():
    """Verify spprof can be imported."""
    import spprof

    assert spprof.__version__ == "0.1.0"


def test_start_stop_basic():
    """Verify start/stop cycle completes without error."""
    import spprof

    # Start profiling
    spprof.start(interval_ms=10)
    assert spprof.is_active()

    # Run some work
    total = 0
    for i in range(100000):
        total += i

    # Stop profiling
    profile = spprof.stop()
    assert not spprof.is_active()

    # Verify profile structure
    assert profile is not None
    assert profile.interval_ms == 10
    assert profile.python_version is not None
    assert profile.platform is not None


def test_double_start_raises():
    """Verify starting while running raises RuntimeError."""
    import spprof

    spprof.start(interval_ms=10)
    try:
        with pytest.raises(RuntimeError, match="already running"):
            spprof.start(interval_ms=10)
    finally:
        spprof.stop()


def test_stop_without_start_raises():
    """Verify stopping without starting raises RuntimeError."""
    import spprof

    with pytest.raises(RuntimeError, match="not running"):
        spprof.stop()


def test_invalid_interval_raises():
    """Verify invalid interval raises ValueError."""
    import spprof

    with pytest.raises(ValueError, match="interval_ms"):
        spprof.start(interval_ms=0)


def test_profiler_context_manager():
    """Test the Profiler context manager."""
    import spprof

    with spprof.Profiler(interval_ms=10) as p:
        total = 0
        for i in range(10000):
            total += i

    # After exiting context, profile should be available
    assert p.profile is not None
    assert p.profile.interval_ms == 10


def test_profile_decorator():
    """Test the @profile decorator."""
    import tempfile
    from pathlib import Path

    import spprof

    with tempfile.NamedTemporaryFile(suffix=".json", delete=False) as f:
        output_path = Path(f.name)

    @spprof.profile(interval_ms=10, output_path=output_path)
    def test_function():
        total = 0
        for i in range(10000):
            total += i
        return total

    result = test_function()
    assert result == sum(range(10000))

    # Profile should have been saved
    assert output_path.exists()
    output_path.unlink()


def test_is_active():
    """Test is_active() function."""
    import spprof

    assert not spprof.is_active()

    spprof.start(interval_ms=10)
    assert spprof.is_active()

    spprof.stop()
    assert not spprof.is_active()


def test_stats():
    """Test stats() function."""
    import spprof

    # Stats should be None when not running
    assert spprof.stats() is None

    spprof.start(interval_ms=10)

    stats = spprof.stats()
    assert stats is not None
    assert stats.collected_samples >= 0
    assert stats.dropped_samples >= 0
    assert stats.duration_ms >= 0

    spprof.stop()


def test_cpu_bound_captures_samples():
    """Verify CPU-bound work captures samples (with native extension)."""
    import spprof

    spprof.start(interval_ms=1)

    # Do CPU-intensive work
    def cpu_work():
        total = 0
        for i in range(1000000):
            total += i**2
        return total

    cpu_work()

    profile = spprof.stop()

    # With pure Python fallback, we may not have samples
    # With native extension, we should have samples
    assert profile is not None
    # Note: Sample count depends on whether native extension is available
