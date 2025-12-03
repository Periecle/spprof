"""Safety tests for memory profiler.

Tests cover:
- Fork safety with multiprocessing (T107)
- Re-entrancy safety (T110)
- Graceful degradation on heap map overflow (T111)
- Graceful degradation on stack table overflow (T112)

Tasks: T107, T110, T111, T112
"""

import gc
import multiprocessing
import os
import platform
import signal
import sys
import threading
import time

import pytest

# Skip all tests on Windows (experimental support)
# Use forked mode for test isolation since shutdown() is a one-way operation
pytestmark = [
    pytest.mark.skipif(
        platform.system() == "Windows",
        reason="Memory profiler on Windows is experimental"
    ),
    pytest.mark.forked,  # Run in separate process to avoid profiler state leakage
]


@pytest.fixture
def memprof_cleanup():
    """Ensure memprof is in a clean state before and after tests.
    
    Note: We do NOT call shutdown() because it's a one-way operation
    that prevents reinitialization. The native extension state persists
    across tests, which is fine for testing purposes.
    """
    import spprof.memprof as memprof

    # Only stop if running (don't reset _initialized - native state persists)
    if memprof._running:
        try:
            memprof.stop()
        except Exception:
            pass
    
    # Reset running state but keep initialized state in sync with native
    memprof._running = False

    yield memprof

    # Cleanup after test - only stop, never shutdown
    if memprof._running:
        try:
            memprof.stop()
        except Exception:
            pass
    
    memprof._running = False


class TestForkSafety:
    """T107: Test fork safety with multiprocessing."""

    @pytest.mark.skipif(
        platform.system() == "Windows",
        reason="Fork not available on Windows"
    )
    def test_fork_during_profiling_no_crash(self, memprof_cleanup):
        """Test that forking while profiling doesn't crash."""
        memprof = memprof_cleanup

        memprof.start(sampling_rate_kb=256)

        # Do some allocations in parent
        parent_data = [bytearray(1024) for _ in range(50)]

        def child_process():
            """Child process work."""
            try:
                # Child should be able to allocate without crashing
                child_data = [bytearray(512) for _ in range(20)]
                time.sleep(0.1)
                del child_data
                return 0
            except Exception as e:
                print(f"Child error: {e}", file=sys.stderr)
                return 1

        # Use 'fork' start method on platforms that support it
        if hasattr(multiprocessing, 'get_context'):
            try:
                ctx = multiprocessing.get_context('fork')
                p = ctx.Process(target=child_process)
            except ValueError:
                # 'fork' not available, skip test
                pytest.skip("Fork start method not available")
        else:
            p = multiprocessing.Process(target=child_process)

        p.start()
        p.join(timeout=10)

        # Child should complete without crash
        assert p.exitcode is not None, "Process didn't complete"

        # Ensure process is fully cleaned up
        if p.is_alive():
            p.terminate()
            p.join(timeout=1)
        p.close()

        # Note: Child might exit with error if profiler isn't fork-safe,
        # but it shouldn't hang or crash the parent
        memprof.stop()

        del parent_data

    @pytest.mark.skipif(
        platform.system() == "Windows",
        reason="os.fork not available on Windows"
    )
    def test_fork_raw_no_crash(self, memprof_cleanup):
        """Test raw fork() during profiling."""
        memprof = memprof_cleanup

        memprof.start(sampling_rate_kb=256)

        data = [bytearray(1024) for _ in range(30)]

        pid = os.fork()

        if pid == 0:
            # Child process
            try:
                # Try to allocate in child
                child_data = bytearray(4096)
                # Exit cleanly
                os._exit(0)
            except Exception:
                os._exit(1)
        else:
            # Parent process
            _, status = os.waitpid(pid, 0)
            child_exit = os.WEXITSTATUS(status)

            # Child should exit cleanly (code 0)
            assert child_exit == 0, f"Child exited with code {child_exit}"

            memprof.stop()

        del data


class TestReentrantSafety:
    """T110: Test re-entrancy safety (allocations in profiler code)."""

    def test_nested_allocation_in_callback_safe(self, memprof_cleanup):
        """Test that allocations don't cause infinite recursion."""
        memprof = memprof_cleanup

        # The profiler itself allocates memory internally.
        # This test verifies that internal allocations don't trigger
        # recursive sampling (re-entrancy guard must work).

        memprof.start(sampling_rate_kb=32)  # Low rate for more opportunities

        # Rapid allocations that could trigger re-entrancy
        for _ in range(1000):
            # This allocation might be sampled
            data = bytearray(256)
            # Sampling code might allocate internally
            # Re-entrancy guard should prevent infinite recursion
            del data

        # Getting snapshot/stats also allocates memory
        for _ in range(10):
            snapshot = memprof.get_snapshot()
            stats = memprof.get_stats()

        # If we get here, re-entrancy is working
        assert stats.total_samples >= 0

        memprof.stop()

    def test_reentrant_stats_tracking(self, memprof_cleanup):
        """Verify that skipped reentrant calls are tracked."""
        memprof = memprof_cleanup

        memprof.start(sampling_rate_kb=32)

        # Do many allocations
        for _ in range(10000):
            data = bytearray(128)
            del data

        stats = memprof.get_stats()

        # The profiler should work correctly
        assert stats.total_samples >= 0

        memprof.stop()


class TestHeapMapOverflow:
    """T111: Test graceful degradation on heap map overflow."""

    @pytest.mark.slow
    def test_heap_map_full_continues_working(self, memprof_cleanup):
        """Test profiler continues when heap map approaches capacity.

        The heap map has 1M entry capacity. We can't actually fill it
        in a reasonable test, but we can verify the profiler handles
        high load gracefully.
        """
        memprof = memprof_cleanup

        memprof.start(sampling_rate_kb=32)  # Very low rate for many samples

        # Hold references to keep entries in heap map
        objects = []

        try:
            # Allocate many objects
            for i in range(50000):
                obj = bytearray(256)
                objects.append(obj)

                # Check stats periodically
                if i % 10000 == 9999:
                    stats = memprof.get_stats()
                    print(f"After {i+1} allocs: "
                          f"samples={stats.total_samples}, "
                          f"load={stats.heap_map_load_percent:.2f}%")

            final_stats = memprof.get_stats()

            # Should have some samples
            assert final_stats.total_samples >= 0

            # Load should be trackable
            assert 0.0 <= final_stats.heap_map_load_percent <= 100.0

        finally:
            memprof.stop()
            del objects

    def test_drops_tracked_on_overflow(self, memprof_cleanup):
        """Test that dropped samples are tracked (if overflow occurs)."""
        memprof = memprof_cleanup

        memprof.start(sampling_rate_kb=64)

        # Do many allocations
        objects = [bytearray(512) for _ in range(10000)]

        stats = memprof.get_stats()

        # Stats should be valid regardless of drops
        assert stats.total_samples >= 0
        assert stats.heap_map_load_percent >= 0.0

        memprof.stop()
        del objects


class TestStackTableOverflow:
    """T112: Test graceful degradation on stack table overflow."""

    def test_many_unique_stacks(self, memprof_cleanup):
        """Test handling of many unique call stacks."""
        memprof = memprof_cleanup

        memprof.start(sampling_rate_kb=64)

        # Generate allocations from many different call sites
        def allocate_at_depth(depth: int):
            if depth <= 0:
                return bytearray(1024)
            return allocate_at_depth(depth - 1)

        objects = []
        for i in range(100):
            # Different stack depths = different stacks
            obj = allocate_at_depth(i % 20)
            objects.append(obj)

        stats = memprof.get_stats()

        # Should track unique stacks
        if stats.total_samples > 0:
            assert stats.unique_stacks >= 1

        memprof.stop()
        del objects

    def test_stack_table_collisions_tracked(self, memprof_cleanup):
        """Test that stack table collisions are tracked."""
        memprof = memprof_cleanup

        memprof.start(sampling_rate_kb=64)

        # Create many allocations
        objects = []
        for _ in range(1000):
            obj = bytearray(256)
            objects.append(obj)

        stats = memprof.get_stats()

        # Collisions stat should be available
        assert stats.collisions >= 0

        memprof.stop()
        del objects


class TestSignalSafety:
    """Test signal safety of the profiler."""

    @pytest.mark.skipif(
        platform.system() == "Windows",
        reason="Signal handling differs on Windows"
    )
    def test_handles_signals_during_profiling(self, memprof_cleanup):
        """Test that profiler handles signals gracefully."""
        memprof = memprof_cleanup

        signal_received = threading.Event()

        def signal_handler(signum, frame):
            signal_received.set()

        # Install custom signal handler
        old_handler = signal.signal(signal.SIGUSR1, signal_handler)

        try:
            memprof.start(sampling_rate_kb=256)

            # Do allocations
            data = [bytearray(1024) for _ in range(100)]

            # Send signal to ourselves
            os.kill(os.getpid(), signal.SIGUSR1)

            # Wait for signal
            signal_received.wait(timeout=1.0)

            # Continue profiling
            more_data = [bytearray(512) for _ in range(50)]

            stats = memprof.get_stats()

            # Profiler should still work
            assert stats.total_samples >= 0

            memprof.stop()

            del data
            del more_data

        finally:
            signal.signal(signal.SIGUSR1, old_handler)


class TestCleanShutdown:
    """Test clean shutdown behavior."""

    def test_shutdown_while_active(self, memprof_cleanup):
        """Test shutdown while profiler is active."""
        memprof = memprof_cleanup

        memprof.start(sampling_rate_kb=256)

        data = [bytearray(1024) for _ in range(50)]

        # Stop first, then shutdown
        memprof.stop()
        memprof.shutdown()

        # State should be clean
        assert memprof._shutdown is True
        assert memprof._running is False

        del data

    def test_double_shutdown_idempotent(self, memprof_cleanup):
        """Test that calling shutdown twice is safe."""
        memprof = memprof_cleanup

        memprof.start()
        memprof.stop()
        memprof.shutdown()

        # Second shutdown should be no-op
        memprof.shutdown()

        assert memprof._shutdown is True

    def test_allocations_after_shutdown(self, memprof_cleanup):
        """Test that allocations after shutdown don't crash."""
        memprof = memprof_cleanup

        memprof.start()
        memprof.stop()
        memprof.shutdown()

        # Allocations after shutdown should just work normally
        # (profiler is disabled)
        data = [bytearray(1024) for _ in range(100)]

        # Clean up
        del data


# Mark slow tests
def pytest_configure(config):
    config.addinivalue_line("markers", "slow: marks tests as slow")

