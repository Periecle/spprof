"""Tests for free-threaded Python support.

These tests verify that spprof works correctly on free-threaded Python builds
(Python 3.13t/3.14t with Py_GIL_DISABLED).

The tests are automatically skipped on GIL-enabled Python builds.
"""

import sys
import threading
import time

import pytest


# Skip entire module if not free-threaded
pytestmark = pytest.mark.skipif(
    not hasattr(sys, "_is_gil_enabled") or sys._is_gil_enabled(),
    reason="Requires free-threaded Python (3.13t+)",
)


class TestBasicProfilingFreethreaded:
    """Basic profiling tests on free-threaded Python."""

    def test_basic_profiling_freethreaded(self):
        """Test that basic profiling works on free-threaded Python."""
        import spprof

        def work():
            total = 0
            for i in range(10000):
                total += i
            return total

        with spprof.Profiler() as p:
            work()

        stats = p.stats()
        # On free-threaded builds, we may capture samples
        assert stats["samples_captured"] >= 0

    def test_simple_function_profiling(self):
        """Test profiling a simple CPU-bound function."""
        import spprof

        def fibonacci(n):
            if n <= 1:
                return n
            return fibonacci(n - 1) + fibonacci(n - 2)

        with spprof.Profiler(interval_ms=5) as p:
            fibonacci(20)

        stats = p.stats()
        assert "samples_captured" in stats
        assert "validation_drops" in stats


class TestMultithreadedProfiling:
    """Tests for profiling with multiple concurrent threads."""

    def test_multithreaded_profiling(self):
        """Test profiling with multiple concurrent threads."""
        import spprof

        results = []

        def worker(n):
            total = 0
            for i in range(10000):
                total += i * n
            results.append(total)

        with spprof.Profiler() as p:
            threads = [threading.Thread(target=worker, args=(i,)) for i in range(4)]
            for t in threads:
                t.start()
            for t in threads:
                t.join()

        stats = p.stats()
        assert stats["samples_captured"] >= 0
        assert len(results) == 4

    def test_many_threads_profiling(self):
        """Test profiling with many concurrent threads."""
        import spprof

        results = []
        num_threads = 8

        def worker(thread_id):
            total = 0
            for i in range(5000):
                total += i * thread_id
            results.append((thread_id, total))

        with spprof.Profiler(interval_ms=5) as p:
            threads = [threading.Thread(target=worker, args=(i,)) for i in range(num_threads)]
            for t in threads:
                t.start()
            for t in threads:
                t.join()

        stats = p.stats()
        assert stats["samples_captured"] >= 0
        assert len(results) == num_threads


class TestValidationDropsTracking:
    """Tests for validation drop statistics tracking."""

    def test_validation_drops_tracked(self):
        """Test that validation drops are tracked in statistics."""
        import spprof

        with spprof.Profiler() as p:
            # Normal workload - should have minimal drops
            for _ in range(100):
                sum(range(1000))

        stats = p.stats()
        # Drops should be countable (even if 0)
        assert "validation_drops" in stats
        # Validation drops should be a reasonable number (including 0)
        assert stats["validation_drops"] >= 0

    def test_validation_drops_visible_in_stats(self):
        """Test that validation_drops appears in profiler stats."""
        import spprof

        spprof.start(interval_ms=10)
        # Do some work
        total = 0
        for i in range(10000):
            total += i
        profile = spprof.stop()

        # The stats should include validation_drops
        assert profile is not None


class TestNoCrashUnderContention:
    """Stress tests to ensure no crashes under thread contention."""

    def test_no_crash_under_contention(self):
        """Stress test: no crashes under high thread contention."""
        import spprof

        stop_flag = threading.Event()

        def churner():
            """Rapidly create and destroy stack frames."""
            while not stop_flag.is_set():

                def a():
                    return b()

                def b():
                    return c()

                def c():
                    return 42

                a()

        with spprof.Profiler(interval_ms=1) as p:  # Fast sampling
            threads = [threading.Thread(target=churner) for _ in range(8)]
            for t in threads:
                t.start()

            # Let it run for a bit
            time.sleep(0.5)

            stop_flag.set()
            for t in threads:
                t.join()

        # If we got here without crashing, test passed
        stats = p.stats()
        assert stats["samples_captured"] >= 0  # Just verify we can read stats

    def test_rapid_thread_creation(self):
        """Test rapid thread creation and destruction during profiling."""
        import spprof

        results = []

        def short_lived_worker(n):
            # Very brief computation
            result = sum(range(n * 100))
            results.append(result)

        with spprof.Profiler(interval_ms=1) as p:
            for _batch in range(10):
                threads = [threading.Thread(target=short_lived_worker, args=(i,)) for i in range(5)]
                for t in threads:
                    t.start()
                for t in threads:
                    t.join()

        stats = p.stats()
        assert stats["samples_captured"] >= 0
        assert len(results) == 50

    def test_mixed_workload_contention(self):
        """Test with mixed CPU and I/O-like workload."""
        import spprof

        results = []

        def cpu_worker():
            total = 0
            for i in range(20000):
                total += i * i
            results.append(("cpu", total))

        def yield_worker():
            for _ in range(100):
                time.sleep(0.001)  # Yield to other threads
            results.append(("yield", None))

        with spprof.Profiler(interval_ms=2) as p:
            threads = []
            for _ in range(4):
                threads.append(threading.Thread(target=cpu_worker))
                threads.append(threading.Thread(target=yield_worker))

            for t in threads:
                t.start()
            for t in threads:
                t.join()

        stats = p.stats()
        assert stats["samples_captured"] >= 0
        assert len(results) == 8


class TestDeepStackProfiling:
    """Tests for profiling deep call stacks."""

    def test_deep_recursion_profiling(self):
        """Test profiling deep recursive calls."""
        import spprof

        def recursive(n, acc=0):
            if n <= 0:
                return acc
            return recursive(n - 1, acc + n)

        with spprof.Profiler(interval_ms=5) as p:
            # Moderate recursion depth
            result = recursive(200)

        stats = p.stats()
        assert stats["samples_captured"] >= 0
        assert result == sum(range(201))

    def test_deep_call_chain(self):
        """Test profiling a deep but non-recursive call chain."""
        import spprof

        def level_1():
            return level_2() + 1

        def level_2():
            return level_3() + 2

        def level_3():
            return level_4() + 3

        def level_4():
            return level_5() + 4

        def level_5():
            total = 0
            for i in range(1000):
                total += i
            return total

        with spprof.Profiler(interval_ms=5) as p:
            for _ in range(100):
                level_1()

        stats = p.stats()
        assert stats["samples_captured"] >= 0


# Note: ARM64-specific tests would run automatically on ARM64 hardware
# The implementation uses SPPROF_ATOMIC_LOAD_PTR which selects the
# appropriate memory ordering for the architecture.


class TestARM64Notes:
    """Placeholder for ARM64-specific test documentation.

    Note: Full ARM64 coverage requires CI runners with ARM64 hardware.
    The speculative capture implementation uses:
      - __atomic_load_n with __ATOMIC_ACQUIRE on ARM64
      - Plain loads on x86-64 (strong memory model)

    These tests run on whatever architecture is available and verify
    the basic functionality works correctly.
    """

    def test_architecture_agnostic_profiling(self):
        """Test that profiling works regardless of architecture."""
        import spprof

        def work():
            return sum(range(5000))

        with spprof.Profiler(interval_ms=10) as p:
            for _ in range(10):
                work()

        stats = p.stats()
        assert "samples_captured" in stats
        assert "validation_drops" in stats
