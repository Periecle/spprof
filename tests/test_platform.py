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
