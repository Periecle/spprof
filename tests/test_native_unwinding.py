"""Tests for native C-stack unwinding."""

import platform

import pytest


def test_native_unwinding_availability():
    """Test checking if native unwinding is available."""
    import spprof

    # Should not raise - just returns True/False
    available = spprof.native_unwinding_available()
    assert isinstance(available, bool)


def test_native_unwinding_default_disabled():
    """Verify native unwinding is disabled by default."""
    import spprof

    assert not spprof.native_unwinding_enabled()


def test_set_native_unwinding_without_availability():
    """Test setting native unwinding when not available."""
    import spprof

    if not spprof.native_unwinding_available():
        # Should raise when trying to enable if not available
        with pytest.raises(RuntimeError):
            spprof.set_native_unwinding(True)

        # Disabling should be fine
        spprof.set_native_unwinding(False)


@pytest.mark.skipif(
    platform.system() == "Windows", reason="Native unwinding not supported on Windows"
)
def test_capture_native_stack_unavailable():
    """Test capturing native stack when native extension not available."""
    import spprof

    # With pure Python fallback, this should raise
    if not hasattr(spprof, "_native") or spprof._native is None:
        with pytest.raises(RuntimeError):
            spprof.capture_native_stack()


def test_native_frame_dataclass():
    """Test NativeFrame dataclass structure."""
    from spprof import NativeFrame

    frame = NativeFrame(
        ip=0x12345678,
        symbol="test_function",
        filename="/lib/test.so",
        offset=0x100,
        resolved=True,
    )

    assert frame.ip == 0x12345678
    assert frame.symbol == "test_function"
    assert frame.filename == "/lib/test.so"
    assert frame.offset == 0x100
    assert frame.resolved is True

    # Should be immutable (frozen dataclass)
    with pytest.raises((AttributeError, TypeError)):  # FrozenInstanceError
        frame.symbol = "modified"  # type: ignore


def test_profiling_works_without_native_unwinding():
    """Verify profiling works even when native unwinding is disabled."""
    import spprof

    # Ensure native unwinding is disabled
    if spprof.native_unwinding_available():
        spprof.set_native_unwinding(False)

    spprof.start(interval_ms=10)

    total = 0
    for i in range(10000):
        total += i

    profile = spprof.stop()
    assert profile is not None


@pytest.mark.skipif(
    platform.system() not in ("Linux", "Darwin"), reason="Native unwinding requires Linux or macOS"
)
def test_native_unwinding_integration():
    """Integration test for native unwinding on supported platforms."""
    import spprof

    if not spprof.native_unwinding_available():
        pytest.skip("Native unwinding not available")

    # Enable native unwinding
    spprof.set_native_unwinding(True)
    assert spprof.native_unwinding_enabled()

    # Profile with native unwinding
    spprof.start(interval_ms=10)

    total = 0
    for i in range(100000):
        total += i * i

    profile = spprof.stop()
    assert profile is not None

    # Disable native unwinding
    spprof.set_native_unwinding(False)
    assert not spprof.native_unwinding_enabled()


@pytest.mark.skipif(
    platform.system() not in ("Linux", "Darwin"), reason="Native unwinding requires Linux or macOS"
)
def test_capture_native_stack():
    """Test capturing the native call stack."""
    import spprof

    if not spprof.native_unwinding_available():
        pytest.skip("Native unwinding not available")

    frames = spprof.capture_native_stack()

    assert isinstance(frames, list)
    # Should have at least a few frames (this function, test runner, etc.)
    assert len(frames) >= 1

    for frame in frames:
        assert isinstance(frame, spprof.NativeFrame)
        assert isinstance(frame.ip, int)
        assert isinstance(frame.symbol, str)
        assert isinstance(frame.resolved, bool)
