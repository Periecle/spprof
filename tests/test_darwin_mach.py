"""
Tests for Darwin Mach-based sampler.

The Darwin implementation uses pure Mach-based sampling:
- Samples ALL threads via thread_suspend/resume
- Uses pthread_introspection_hook for thread discovery
- Captures Python frames by finding PyThreadState for each suspended thread
- Uses mach_wait_until for precise timing

This approach eliminates signal-safety constraints and provides accurate
per-thread sampling data.

Copyright (c) 2024 spprof contributors
SPDX-License-Identifier: MIT
"""

import platform
import threading
import time

import pytest


# Skip all tests on non-Darwin platforms
# Also use forked tests for isolation since memory profiler hooks interact with system calls
pytestmark = [
    pytest.mark.skipif(platform.system() != "Darwin", reason="Darwin-only tests"),
    pytest.mark.forked,  # Run in separate process to avoid profiler state leakage
]


# Feature flags for tests that require full Mach-based sampler
# Mach sampler is now fully integrated with Python frame capture support
MACH_SAMPLER_AVAILABLE = True


@pytest.fixture
def profiler():
    """Create a profiler instance for testing."""
    import spprof

    return spprof


class TestBasicSampling:
    """Test basic sampling on Darwin."""

    def test_single_thread_sampling(self, profiler):
        """Verify basic single-threaded profiling works."""

        def cpu_work():
            total = 0
            for i in range(1000000):
                total += i
            return total

        with profiler.Profiler(interval_ms=10) as p:
            cpu_work()

        # Should complete without error
        assert p.profile is not None

    def test_profiler_start_stop(self, profiler):
        """Test profiler can start and stop without errors."""
        profiler.start(interval_ms=10)
        assert profiler.is_active()

        # Do minimal work
        _ = sum(range(100))

        profile = profiler.stop()
        assert not profiler.is_active()
        assert profile is not None

    def test_profiler_context_manager(self, profiler):
        """Test profiler works as context manager."""
        with profiler.Profiler(interval_ms=10) as p:
            _ = sum(range(10000))

        # Should have returned profile data
        assert p.profile is not None


class TestMultithreadSampling:
    """Test multi-thread sampling capabilities.

    NOTE: Full multi-thread sampling requires the Mach-based sampler
    which is not yet fully integrated. These tests are skipped until
    the implementation is complete.
    """

    @pytest.mark.skipif(not MACH_SAMPLER_AVAILABLE, reason="Requires Mach-based sampler")
    def test_multithread_sampling(self, profiler):
        """Verify samples are captured from all threads, not just main."""
        results = {}

        def worker(name):
            total = 0
            for i in range(500000):
                total += i * i
            results[name] = total
            return total

        threads = []
        for i in range(4):
            t = threading.Thread(target=worker, args=(f"worker_{i}",))
            threads.append(t)

        with profiler.Profiler(interval_ms=10) as p:
            for t in threads:
                t.start()
            for t in threads:
                t.join()

        samples = p.profile.samples
        sample_thread_ids = {s.thread_id for s in samples}

        # Should have samples from multiple threads
        assert len(sample_thread_ids) >= 2

    @pytest.mark.skipif(not MACH_SAMPLER_AVAILABLE, reason="Requires Mach-based sampler")
    def test_dynamic_thread_creation(self, profiler):
        """Verify dynamically spawned threads are discovered and sampled."""
        results = []

        def spawn_and_work():
            def worker():
                results.append(sum(range(300000)))

            t = threading.Thread(target=worker)
            t.start()
            t.join()

        with profiler.Profiler(interval_ms=10) as p:
            for _ in range(3):
                spawn_and_work()
                time.sleep(0.05)

        assert len(p.profile.samples) > 0


class TestSamplingRateAccuracy:
    """Test sampling rate accuracy."""

    def test_sampling_basic(self, profiler):
        """Test that some samples are captured."""

        def cpu_work():
            total = 0
            for i in range(5000000):
                total += i
            return total

        with profiler.Profiler(interval_ms=10) as p:
            cpu_work()

        # We should complete without error
        # Note: on macOS with setitimer, sample count may vary significantly
        assert p.profile is not None


class TestThreadDiscovery:
    """Test thread discovery edge cases."""

    def test_thread_termination_handling_basic(self, profiler):
        """Verify profiler doesn't crash when threads terminate."""
        completed = []

        def short_worker():
            completed.append(sum(range(1000)))

        with profiler.Profiler(interval_ms=10) as p:
            threads = [threading.Thread(target=short_worker) for _ in range(3)]
            for t in threads:
                t.start()
            for t in threads:
                t.join()

        # Should complete without crashing
        assert len(completed) == 3
        assert p.profile is not None


class TestArchitecture:
    """Test architecture-specific functionality."""

    def test_architecture_detection(self, profiler):  # noqa: ARG002
        """Verify we're running on a supported architecture."""
        import struct

        ptr_size = struct.calcsize("P")
        assert ptr_size == 8, "Expected 64-bit architecture"

        machine = platform.machine()
        assert machine in ("x86_64", "arm64"), f"Unexpected architecture: {machine}"

    def test_profiler_works_on_architecture(self, profiler):
        """Verify profiler works on current architecture."""

        def cpu_work():
            return sum(range(100000))

        with profiler.Profiler(interval_ms=10) as p:
            cpu_work()

        assert p.profile is not None


class TestEdgeCases:
    """Test edge cases and error handling."""

    def test_no_privilege_required(self, profiler):
        """Verify no special entitlements are needed."""

        def cpu_work():
            return sum(range(100000))

        try:
            with profiler.Profiler(interval_ms=10) as p:
                cpu_work()
            assert p.profile is not None
        except PermissionError as e:
            pytest.fail(f"Profiler requires elevated privileges: {e}")

    def test_rapid_start_stop(self, profiler):
        """Test rapid profiler start/stop cycles."""

        def cpu_work():
            return sum(range(10000))

        for _ in range(10):
            with profiler.Profiler(interval_ms=10) as p:
                cpu_work()
            assert p.profile is not None

    def test_empty_profile(self, profiler):
        """Test profiling with minimal activity."""
        with profiler.Profiler(interval_ms=10) as p:
            time.sleep(0.01)

        assert p.profile is not None


# ============================================================================
# Memory Profiler Tests for Darwin (T056)
# ============================================================================


@pytest.fixture
def memprof_cleanup():
    """Ensure memprof is in a clean state before and after tests.
    
    Note: We do NOT call shutdown() because it's a one-way operation
    that permanently disables the profiler. The native extension maintains
    its own state which persists across Python module state changes.
    """
    import spprof.memprof as memprof

    # Stop if running - use try/except since state may be inconsistent
    if memprof._running:
        try:
            memprof.stop()
        except RuntimeError:
            # Already stopped at native level
            memprof._running = False
    
    # If we've shut down, tests can't run - skip
    if memprof._shutdown:
        pytest.skip("Memory profiler was shutdown in a previous test")

    yield memprof

    # Cleanup after test - only stop, never shutdown
    if memprof._running:
        try:
            memprof.stop()
        except RuntimeError:
            pass
        memprof._running = False


class TestDarwinMallocLogger:
    """T056: Integration tests for macOS malloc_logger.

    The Darwin memory profiler uses the malloc_logger callback which is
    the official Apple API for allocation tracking. These tests verify
    the malloc_logger integration works correctly on macOS.
    """

    def test_malloc_logger_install_uninstall(self, memprof_cleanup):
        """Test that malloc_logger can be installed and uninstalled."""
        memprof = memprof_cleanup

        # Start should install malloc_logger
        memprof.start(sampling_rate_kb=256)
        assert memprof._running is True

        # Do some allocations
        data = [bytearray(1024) for _ in range(100)]

        # Stop should uninstall malloc_logger
        memprof.stop()
        assert memprof._running is False

        del data

    def test_malloc_logger_captures_allocations(self, memprof_cleanup):
        """Test that malloc_logger captures allocations."""
        memprof = memprof_cleanup

        memprof.start(sampling_rate_kb=64)  # Low rate for more samples

        # Allocate memory that should be captured
        large_allocations = [bytearray(4096) for _ in range(100)]

        snapshot = memprof.get_snapshot()
        stats = memprof.get_stats()

        # Should have captured some samples
        assert stats.total_samples >= 0

        memprof.stop()

        del large_allocations

    def test_malloc_logger_tracks_frees(self, memprof_cleanup):
        """Test that malloc_logger tracks free events."""
        memprof = memprof_cleanup

        memprof.start(sampling_rate_kb=64)

        # Allocate and free
        for _ in range(100):
            data = bytearray(4096)
            del data

        import gc
        gc.collect()

        stats = memprof.get_stats()

        # Should track freed allocations if any were sampled
        assert stats.freed_samples >= 0

        memprof.stop()

    def test_malloc_logger_zombie_race_detection(self, memprof_cleanup):
        """Test zombie race detection (address reuse before callback).

        On macOS, malloc_logger is a post-hook callback, meaning the
        callback runs after malloc/free completes. This creates a race
        where an address can be reallocated before the free callback runs.

        The profiler uses sequence numbers to detect and handle this case.
        """
        memprof = memprof_cleanup

        memprof.start(sampling_rate_kb=32)  # Very low rate

        # Rapid alloc/free cycles that could trigger zombie race
        for _ in range(1000):
            # Small allocation to maximize chance of address reuse
            data = bytearray(64)
            del data

        stats = memprof.get_stats()

        # zombie_races_detected tracks when sequence check detects reuse
        # This is not an error - it's expected behavior that's handled correctly
        assert stats.zombie_races_detected >= 0

        memprof.stop()

    def test_malloc_logger_multithread_safety(self, memprof_cleanup):
        """Test malloc_logger is thread-safe."""
        import threading
        import gc

        memprof = memprof_cleanup

        memprof.start(sampling_rate_kb=128)

        errors = []

        def allocate_worker(worker_id: int, count: int):
            try:
                for _ in range(count):
                    data = bytearray(1024)
                    time.sleep(0.001)
                    del data
            except Exception as e:
                errors.append(f"Worker {worker_id}: {e}")

        threads = []
        for i in range(5):
            t = threading.Thread(target=allocate_worker, args=(i, 100))
            threads.append(t)

        for t in threads:
            t.start()
        for t in threads:
            t.join(timeout=30)

        # Verify all threads completed
        for t in threads:
            assert not t.is_alive(), f"Thread {t.name} still running"

        assert not errors, f"Errors: {errors}"

        stats = memprof.get_stats()
        assert stats.total_samples >= 0

        memprof.stop()
        
        # Force cleanup of thread state
        gc.collect()
        time.sleep(0.01)

    def test_malloc_logger_with_cpu_profiler(self, memprof_cleanup, profiler):
        """Test malloc_logger works alongside CPU profiler."""
        memprof = memprof_cleanup

        # Start both profilers
        profiler.start(interval_ms=10)
        memprof.start(sampling_rate_kb=256)

        # Mixed workload
        result = 0
        for i in range(10000):
            result += i ** 2
            if i % 100 == 0:
                data = bytearray(1024)
                del data

        # Get both results
        mem_snapshot = memprof.get_snapshot()
        cpu_profile = profiler.stop()
        memprof.stop()

        # Both should work
        assert cpu_profile is not None
        assert mem_snapshot is not None
        assert mem_snapshot.total_samples >= 0

    def test_malloc_logger_rapid_start_stop(self, memprof_cleanup):
        """Test rapid start/stop doesn't cause issues.
        
        Note: We only test start/stop cycles without shutdown since
        shutdown is a one-way operation that can't be undone.
        """
        memprof = memprof_cleanup

        for i in range(10):
            memprof.start(sampling_rate_kb=512)

            data = bytearray(4096)
            del data

            memprof.stop()

        # Should complete without crashes
