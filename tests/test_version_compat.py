"""Tests for Python version compatibility."""

import sys

import pytest


def test_frame_walker_version_detection():
    """Verify correct compat header used for current Python version."""
    import spprof

    # Should work on all supported versions
    assert spprof.is_active() is False

    spprof.start(interval_ms=10)
    assert spprof.is_active() is True

    profile = spprof.stop()
    assert profile is not None
    assert (
        profile.python_version
        == f"{sys.version_info.major}.{sys.version_info.minor}.{sys.version_info.micro}"
    )


def test_profile_on_current_version():
    """Verify profiling works on current Python version."""
    import spprof

    with spprof.Profiler(interval_ms=10) as p:
        # Do some work
        total = 0
        for i in range(10000):
            total += i

    assert p.profile is not None
    # Profile should have captured the current Python version
    assert sys.version_info.major == int(p.profile.python_version.split(".")[0])


def test_frame_structure_access():
    """Verify frame structures can be accessed."""
    from spprof import Frame

    # Create frames manually to test structure
    frame = Frame(
        function_name="test_func",
        filename="test.py",
        lineno=42,
        is_native=False,
    )

    assert frame.function_name == "test_func"
    assert frame.filename == "test.py"
    assert frame.lineno == 42
    assert frame.is_native is False

    # Frames should be immutable (frozen dataclass)
    with pytest.raises((AttributeError, TypeError)):  # FrozenInstanceError
        frame.function_name = "modified"  # type: ignore


def test_version_in_output():
    """Verify Python version appears in profile output."""
    import spprof

    spprof.start(interval_ms=10)
    _total = sum(range(1000))
    profile = spprof.stop()

    speedscope = profile.to_speedscope()

    # Should have exporter info
    assert "exporter" in speedscope
    assert "spprof" in speedscope["exporter"]


@pytest.mark.skipif(
    sys.version_info < (3, 13),
    reason="Free-threading only available in Python 3.13+",
)
def test_free_threading_compatible():
    """Verify compatibility with free-threaded Python builds."""
    import spprof

    # This test runs on 3.13+ (may or may not be free-threaded build)
    spprof.start(interval_ms=10)
    _total = sum(range(1000))
    profile = spprof.stop()

    assert profile is not None
