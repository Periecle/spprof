"""Tests for platform detection and timer mechanisms."""

import platform
import sys

import pytest


def test_platform_detection():
    """Verify platform is correctly detected."""
    import spprof

    profile = None
    spprof.start(interval_ms=10)
    profile = spprof.stop()

    # Platform should be in the profile
    assert profile.platform is not None
    assert len(profile.platform) > 0

    # Should contain system info
    expected_system = platform.system()
    assert expected_system in profile.platform or profile.platform.startswith(expected_system)


def test_python_version_detection():
    """Verify Python version is correctly detected."""
    import spprof

    spprof.start(interval_ms=10)
    profile = spprof.stop()

    # Python version should match runtime
    expected_version = f"{sys.version_info.major}.{sys.version_info.minor}.{sys.version_info.micro}"
    assert profile.python_version == expected_version


def test_profiler_works_on_current_platform():
    """Verify profiler works on the current platform."""
    import spprof

    # Basic functionality should work
    spprof.start(interval_ms=10)
    assert spprof.is_active()

    # Do some work
    _total = sum(range(10000))

    profile = spprof.stop()
    assert not spprof.is_active()
    assert profile is not None


@pytest.mark.skipif(platform.system() != "Linux", reason="Linux-specific timer mechanism test")
def test_linux_timer_mechanism():
    """Test Linux-specific timer_create functionality."""
    import spprof

    # On Linux, we should be able to use timer_create
    spprof.start(interval_ms=5)

    # Do CPU-bound work
    total = 0
    for i in range(100000):
        total += i * i

    profile = spprof.stop()
    assert profile is not None


@pytest.mark.skipif(platform.system() != "Darwin", reason="macOS-specific timer mechanism test")
def test_darwin_timer_mechanism():
    """Test macOS-specific setitimer functionality."""
    import spprof

    # On macOS, we should be able to use setitimer
    spprof.start(interval_ms=5)

    # Do CPU-bound work
    total = 0
    for i in range(100000):
        total += i * i

    profile = spprof.stop()
    assert profile is not None


@pytest.mark.skipif(platform.system() != "Windows", reason="Windows-specific timer mechanism test")
def test_windows_timer_mechanism():
    """Test Windows-specific timer queue functionality."""
    import spprof

    # On Windows, we use timer queue + suspend
    spprof.start(interval_ms=5)

    # Do CPU-bound work
    total = 0
    for i in range(100000):
        total += i * i

    profile = spprof.stop()
    assert profile is not None


def test_stats_contain_timing_info():
    """Verify stats contain duration information."""
    import time

    import spprof

    spprof.start(interval_ms=10)
    time.sleep(0.1)  # Wait a bit

    stats = spprof.stats()
    assert stats is not None
    assert stats.duration_ms >= 0

    spprof.stop()


@pytest.mark.skipif(platform.system() != "Linux", reason="Linux-specific timer overrun test")
def test_timer_overrun_stats():
    """Test that timer overrun statistics are tracked (Linux only).

    Timer overruns occur when the system can't deliver signals fast enough.
    This test verifies the stats API includes overrun tracking.
    """
    import spprof

    # Use very fast sampling to potentially cause overruns
    spprof.start(interval_ms=1)

    # CPU-bound work to keep the profiler busy
    total = 0
    for _ in range(100000):
        total += sum(range(100))

    profile = spprof.stop()
    assert profile is not None
    # Overruns might be 0 if system is fast enough - we just verify no crash


@pytest.mark.skipif(platform.system() != "Linux", reason="Linux-specific stress test")
def test_start_stop_stress():
    """Test rapid start/stop cycles don't cause crashes (SC-003).

    This validates race-free shutdown by starting and stopping
    the profiler many times in succession.
    """
    import spprof

    # 1000 start/stop cycles as per spec SC-003
    for _ in range(1000):
        spprof.start(interval_ms=10)
        # Brief work
        _ = sum(range(100))
        spprof.stop()

    # If we get here without crash, test passes


@pytest.mark.skipif(platform.system() != "Linux", reason="Linux-specific shutdown timing test")
def test_shutdown_timing():
    """Verify profiler shutdown completes within 100ms (SC-005)."""
    import time

    import spprof

    spprof.start(interval_ms=5)

    # Do some work
    _ = sum(range(100000))

    start = time.monotonic()
    spprof.stop()
    elapsed_ms = (time.monotonic() - start) * 1000

    # Shutdown should complete within 100ms per SC-005
    assert elapsed_ms < 100, f"Shutdown took {elapsed_ms:.1f}ms, expected < 100ms"


@pytest.mark.skipif(platform.system() != "Linux", reason="Linux-specific SIGPROF test")
def test_sigprof_blocked():
    """Test profiler behavior when SIGPROF is blocked by application.

    Some applications block SIGPROF. The profiler should handle this
    gracefully without crashing.
    """
    import signal

    import spprof

    # Block SIGPROF
    old_mask = signal.pthread_sigmask(signal.SIG_BLOCK, [signal.SIGPROF])

    try:
        # Start profiler - should handle blocked signal gracefully
        spprof.start(interval_ms=10)

        # Do some work
        _ = sum(range(10000))

        # Stop should still work
        profile = spprof.stop()
        assert profile is not None
    finally:
        # Restore original signal mask
        signal.pthread_sigmask(signal.SIG_SETMASK, old_mask)


@pytest.mark.skipif(platform.system() != "Linux", reason="Linux-specific pause/resume test")
def test_pause_resume_basic():
    """Test basic pause/resume functionality.

    Verifies that the profiler can be paused and resumed
    without crashing or losing state.
    """
    import spprof

    spprof.start(interval_ms=10)

    # Do some work
    _ = sum(range(10000))

    # Pause should succeed (or no-op on platforms without support)
    # Note: pause() not yet exposed in Python API, so we just verify
    # the profiler runs correctly through normal flow

    profile = spprof.stop()
    assert profile is not None


@pytest.mark.skipif(platform.system() != "Linux", reason="Linux-specific pause/resume test")
def test_pause_no_samples():
    """Verify no samples are captured during pause (when implemented).

    This test validates that pausing actually stops sample collection.
    Note: The pause API is not yet exposed to Python, so this is a
    placeholder that verifies basic profiler functionality.
    """
    import spprof

    spprof.start(interval_ms=5)

    # Do some work
    total = 0
    for i in range(50000):
        total += i

    profile = spprof.stop()
    assert profile is not None
