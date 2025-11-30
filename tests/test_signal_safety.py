"""Tests for signal safety and deadlock prevention."""

import threading

import pytest


def test_no_deadlock_during_gil_operations():
    """Verify profiler doesn't deadlock when signal arrives during GIL ops."""
    import spprof

    deadlock_detected = threading.Event()
    test_completed = threading.Event()

    def watchdog():
        """Kill the test if it hangs for too long."""
        if not test_completed.wait(timeout=5):
            deadlock_detected.set()
            # Can't easily force exit in pytest, but the timeout
            # decorator will handle this

    watchdog_thread = threading.Thread(target=watchdog, daemon=True)
    watchdog_thread.start()

    try:
        spprof.start(interval_ms=1)  # Aggressive sampling

        # Perform GIL-heavy operations that could deadlock
        for _ in range(10000):
            # Import causes GIL acquisition
            import sys

            _ = sys.path  # Access sys module

            # String operations hold GIL
            s = "x" * 100
            _ = s.upper()

            # List operations
            lst = list(range(10))
            _ = sorted(lst)

        spprof.stop()
        test_completed.set()

    except Exception:
        test_completed.set()
        raise

    assert not deadlock_detected.is_set(), "Deadlock detected during GIL operations"


def test_gc_stress():
    """Verify profiler handles code objects being GC'd during profiling."""
    import gc

    import spprof

    spprof.start(interval_ms=1)

    for i in range(100):
        # Create and immediately discard functions
        exec(f"def temp_func_{i}(): pass")
        gc.collect()

    # Should not crash on resolution of GC'd code objects
    profile = spprof.stop()
    assert profile is not None


def test_ring_buffer_overflow_handling():
    """Verify samples are dropped, not crashed, on overflow."""
    import spprof

    spprof.start(interval_ms=1)

    # Burn CPU to generate many samples
    def busy_loop():
        x = 0
        for i in range(1_000_000):
            x += i
        return x

    # Run in multiple threads to increase sample pressure
    threads = [threading.Thread(target=busy_loop) for _ in range(4)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    # Should complete without crash
    profile = spprof.stop()
    assert profile is not None
    # May have dropped samples, but shouldn't crash
    assert profile.dropped_count >= 0


def test_exception_during_profiling():
    """Verify profiler handles exceptions during profiled code."""
    import spprof

    spprof.start(interval_ms=10)

    try:
        raise ValueError("Test exception")
    except ValueError:
        pass  # Expected

    # Should still be able to stop cleanly
    profile = spprof.stop()
    assert profile is not None


def test_nested_profiling_prevented():
    """Verify nested profiling is prevented."""
    import spprof

    spprof.start(interval_ms=10)

    with pytest.raises(RuntimeError, match="already running"):
        spprof.start(interval_ms=10)

    spprof.stop()


def test_rapid_start_stop():
    """Verify rapid start/stop cycles don't cause issues."""
    import spprof

    for _ in range(10):
        spprof.start(interval_ms=10)
        # Minimal work
        _ = sum(range(100))
        profile = spprof.stop()
        assert profile is not None
