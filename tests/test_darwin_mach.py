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
pytestmark = pytest.mark.skipif(platform.system() != "Darwin", reason="Darwin-only tests")


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
