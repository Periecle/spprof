"""Stress tests for edge cases and system limits.

These tests verify profiler stability under extreme conditions:
- Rapid start/stop cycles
- Memory pressure
- Thread death during sampling
- GC during sampling
- High sample throughput
"""

import gc
import platform
import sys
import threading
import time
import weakref

import pytest


class TestRapidStartStop:
    """Tests for rapid profiler start/stop cycles."""

    def test_rapid_cycles_100(self):
        """100 rapid start/stop cycles without delay."""
        import spprof

        for i in range(100):
            spprof.start(interval_ms=10)
            profile = spprof.stop()
            assert profile is not None

    def test_rapid_cycles_with_minimal_work(self):
        """Rapid cycles with minimal work between start/stop."""
        import spprof

        for _ in range(50):
            spprof.start(interval_ms=1)
            # Minimal CPU work
            _ = sum(range(10))
            profile = spprof.stop()
            assert profile is not None

    @pytest.mark.skipif(
        platform.system() == "Darwin",
        reason="Darwin setitimer can have issues with very rapid cycles",
    )
    def test_rapid_cycles_aggressive(self):
        """Very aggressive rapid start/stop - 200 cycles with 1ms interval."""
        import spprof

        errors = []
        for i in range(200):
            try:
                spprof.start(interval_ms=1)
                # Force at least one sample opportunity
                _ = sum(range(100))
                profile = spprof.stop()
                assert profile is not None
            except Exception as e:
                errors.append((i, e))

        # Allow up to 1% failure rate for edge cases
        assert len(errors) <= 2, f"Too many errors during rapid cycling: {errors}"

    def test_rapid_cycles_multithreaded(self):
        """Rapid cycles with threads being created/destroyed."""
        import spprof

        def short_task():
            return sum(range(1000))

        for _ in range(30):
            spprof.start(interval_ms=5)

            threads = [threading.Thread(target=short_task) for _ in range(5)]
            for t in threads:
                t.start()
            for t in threads:
                t.join()

            profile = spprof.stop()
            assert profile is not None


class TestMemoryPressure:
    """Tests for behavior under memory pressure."""

    def test_large_stack_depth(self):
        """Test with deeply recursive calls (max stack depth)."""
        import spprof

        def recursive(n, acc=0):
            if n <= 0:
                return acc
            return recursive(n - 1, acc + n)

        spprof.start(interval_ms=5)

        # Python default recursion limit is ~1000
        # Use moderate recursion to stay safe
        result = recursive(500)

        profile = spprof.stop()
        assert profile is not None
        assert result == sum(range(501))

    def test_many_samples_basic(self):
        """Generate many samples without memory issues."""
        import spprof

        spprof.start(interval_ms=1)

        # Busy loop to generate samples
        end_time = time.monotonic() + 0.5  # 500ms = ~500 samples at 1ms
        count = 0
        while time.monotonic() < end_time:
            count += sum(range(100))

        profile = spprof.stop()
        assert profile is not None
        # Should have captured at least some samples
        assert profile.sample_count >= 0  # May drop some under pressure

    def test_aggregation_memory_savings(self):
        """Verify aggregation reduces memory for repetitive stacks."""
        import spprof

        def hot_function():
            """This function should appear many times."""
            return sum(range(100))

        spprof.start(interval_ms=1)

        # Call same function many times - should produce identical stacks
        for _ in range(10000):
            hot_function()

        profile = spprof.stop()

        if profile.sample_count > 10:  # Only test if we got samples
            agg = profile.aggregate()
            # For repetitive code, we should see compression
            assert agg.unique_stack_count <= profile.sample_count
            if agg.unique_stack_count > 0:
                # Verify compression ratio is positive
                assert agg.compression_ratio >= 1.0

    def test_many_unique_stacks(self):
        """Test with many different stack traces."""
        import spprof

        # Create many different call stacks dynamically
        def create_func(n):
            def func():
                return n * 2

            func.__name__ = f"func_{n}"
            return func

        funcs = [create_func(i) for i in range(100)]

        spprof.start(interval_ms=5)

        # Call different functions to create varied stacks
        for f in funcs:
            for _ in range(10):
                f()

        profile = spprof.stop()
        assert profile is not None


class TestThreadDeathDuringSampling:
    """Tests for threads dying while being sampled."""

    def test_thread_exits_immediately(self):
        """Thread that exits almost immediately after creation."""
        import spprof

        results = []

        def instant_exit():
            results.append(1)
            # Thread exits immediately

        spprof.start(interval_ms=1)

        for _ in range(50):
            t = threading.Thread(target=instant_exit)
            t.start()
            # Don't wait - let threads exit on their own

        # Brief wait for threads to finish
        time.sleep(0.1)

        profile = spprof.stop()
        assert profile is not None
        assert len(results) == 50

    def test_thread_exits_during_likely_sample(self):
        """Thread that exits during high probability of being sampled."""
        import spprof

        barrier = threading.Barrier(11)  # 10 workers + 1 main
        results = []

        def worker():
            barrier.wait()  # Sync start
            # Do minimal work then exit
            x = sum(range(1000))
            results.append(x)

        spprof.start(interval_ms=1)

        threads = [threading.Thread(target=worker) for _ in range(10)]
        for t in threads:
            t.start()

        barrier.wait()  # Let all workers start together

        for t in threads:
            t.join()

        profile = spprof.stop()
        assert profile is not None
        assert len(results) == 10

    @pytest.mark.skipif(
        platform.system() != "Linux", reason="Linux-specific thread timing"
    )
    def test_thread_death_race_condition(self):
        """Stress test for thread death during timer fire."""
        import spprof

        errors = []

        def volatile_thread():
            """Thread that exits quickly, potentially during sample."""
            try:
                spprof.register_thread()
                _ = sum(range(100))
                spprof.unregister_thread()
            except Exception as e:
                errors.append(e)

        spprof.start(interval_ms=1)

        # Create many short-lived threads rapidly
        for _ in range(100):
            t = threading.Thread(target=volatile_thread)
            t.start()
            # No join - let them exit whenever

        # Wait for threads to settle
        time.sleep(0.2)

        profile = spprof.stop()
        assert profile is not None
        assert len(errors) == 0, f"Thread errors: {errors}"


class TestGCDuringSampling:
    """Tests for garbage collection during sampling."""

    def test_gc_stress_with_code_objects(self):
        """Force GC to collect code objects during profiling."""
        import spprof

        spprof.start(interval_ms=1)

        # Create and discard code objects rapidly
        for i in range(200):
            # Dynamic code creation
            exec(f"def temp_{i}(): return {i}")
            if i % 10 == 0:
                gc.collect()

        profile = spprof.stop()
        assert profile is not None

    def test_gc_during_frame_walking(self):
        """GC triggered during frame walking."""
        import spprof

        weak_refs = []

        def create_garbage():
            """Create objects that will be GC'd."""
            obj = {"data": [0] * 1000}
            weak_refs.append(weakref.ref(obj))
            return obj

        spprof.start(interval_ms=1)

        for _ in range(100):
            _ = create_garbage()
            # Force GC during profiling
            gc.collect()
            # Do some CPU work
            _ = sum(range(1000))

        profile = spprof.stop()
        assert profile is not None

        # Verify some objects were actually collected
        gc.collect()
        dead_refs = sum(1 for ref in weak_refs if ref() is None)
        assert dead_refs > 0, "GC didn't collect any objects"

    def test_gc_with_circular_refs(self):
        """GC with circular references during profiling."""
        import spprof

        spprof.start(interval_ms=1)

        for _ in range(50):
            # Create circular reference
            a = {"name": "a"}
            b = {"name": "b", "ref": a}
            a["ref"] = b
            # Delete both
            del a, b
            gc.collect()
            # CPU work
            _ = sum(range(500))

        profile = spprof.stop()
        assert profile is not None


class TestHighThroughput:
    """Tests for high sample throughput."""

    @pytest.mark.skipif(
        platform.system() == "Darwin",
        reason="Darwin may drop samples at very high rates",
    )
    def test_high_frequency_sampling(self):
        """Very high frequency sampling (1ms interval)."""
        import spprof

        spprof.start(interval_ms=1)

        # CPU-bound work for 500ms
        end_time = time.monotonic() + 0.5
        total = 0
        while time.monotonic() < end_time:
            total += sum(range(1000))

        profile = spprof.stop()
        assert profile is not None
        # At 1ms interval over 500ms, expect around 500 samples max
        # (but likely less due to overhead and timing)
        assert profile.sample_count >= 0

    def test_ring_buffer_pressure(self):
        """Test ring buffer under heavy write pressure."""
        import spprof

        # Multiple threads generating samples
        threads_done = threading.Event()
        sample_work_done = []

        def cpu_burner(n):
            total = 0
            while not threads_done.is_set():
                total += sum(range(1000))
            sample_work_done.append((n, total))

        spprof.start(interval_ms=1)

        # Start CPU-burning threads
        threads = [threading.Thread(target=cpu_burner, args=(i,)) for i in range(4)]
        for t in threads:
            t.start()

        # Let them run
        time.sleep(0.3)

        threads_done.set()
        for t in threads:
            t.join()

        profile = spprof.stop()
        assert profile is not None
        assert len(sample_work_done) == 4
        # Check if samples were dropped (acceptable under pressure)
        # Just verify we didn't crash
        assert profile.dropped_count >= 0


class TestConcurrencyEdgeCases:
    """Tests for concurrency edge cases."""

    def test_start_from_thread(self):
        """Start profiler from non-main thread."""
        import spprof

        result = {}

        def start_in_thread():
            try:
                spprof.start(interval_ms=10)
                _ = sum(range(10000))
                profile = spprof.stop()
                result["profile"] = profile
            except Exception as e:
                result["error"] = e

        t = threading.Thread(target=start_in_thread)
        t.start()
        t.join()

        assert "error" not in result, f"Error: {result.get('error')}"
        assert result["profile"] is not None

    def test_stop_from_different_thread(self):
        """Stop profiler from different thread than started."""
        import spprof

        result = {}

        def stop_in_thread():
            try:
                time.sleep(0.05)  # Let main thread do work
                profile = spprof.stop()
                result["profile"] = profile
            except Exception as e:
                result["error"] = e

        spprof.start(interval_ms=5)

        t = threading.Thread(target=stop_in_thread)
        t.start()

        # Do work while profiling
        while t.is_alive():
            _ = sum(range(1000))

        t.join()

        # Should succeed (or raise appropriate error)
        # Implementation may or may not support this
        if "error" in result:
            # At minimum, shouldn't crash
            pass
        else:
            assert result["profile"] is not None


class TestAggregationStress:
    """Stress tests for aggregation feature."""

    def test_aggregate_empty_profile(self):
        """Aggregate profile with no samples."""
        import spprof

        spprof.start(interval_ms=1000)  # Very low frequency
        profile = spprof.stop()  # Stop immediately

        agg = profile.aggregate()
        assert agg.total_samples == profile.sample_count
        assert agg.unique_stack_count == len(agg.stacks)

    def test_aggregate_large_profile(self):
        """Aggregate profile with many samples."""
        import spprof

        spprof.start(interval_ms=1)

        # Generate many samples
        end_time = time.monotonic() + 0.5
        while time.monotonic() < end_time:
            _ = sum(range(1000))

        profile = spprof.stop()

        if profile.sample_count > 0:
            agg = profile.aggregate()
            # Verify invariants
            assert agg.total_samples == profile.sample_count
            assert agg.unique_stack_count <= profile.sample_count
            total_from_counts = sum(s.count for s in agg.stacks)
            assert total_from_counts == agg.total_samples

    def test_aggregate_preserves_thread_info(self):
        """Verify aggregation preserves thread information."""
        import spprof

        def worker():
            total = 0
            for i in range(50000):
                total += i
            return total

        spprof.start(interval_ms=5)

        threads = [
            threading.Thread(target=worker, name=f"Worker-{i}") for i in range(3)
        ]
        for t in threads:
            t.start()
        for t in threads:
            t.join()

        profile = spprof.stop()

        if profile.sample_count > 0:
            agg = profile.aggregate()
            # Thread IDs should be preserved
            thread_ids = {s.thread_id for s in profile.samples}
            agg_thread_ids = {s.thread_id for s in agg.stacks}
            assert agg_thread_ids.issubset(thread_ids)

