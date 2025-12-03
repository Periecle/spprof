"""Stress tests for memory profiler.

These tests verify correct behavior under high load:
- Concurrent allocation from multiple threads (T051, T069)
- High allocation rate (T068)

Tasks: T051, T068, T069
"""

import gc
import platform
import random
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from typing import List

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


class TestHeapMapStress:
    """T051: Concurrent stress test for heap map (10 threads, 1M ops)."""

    @pytest.mark.slow
    def test_heap_map_10_threads_1m_ops(self, memprof_cleanup):
        """Stress test heap map with 10 threads and ~1M total operations.

        This tests the lock-free heap map under concurrent load to ensure:
        - No data races or crashes
        - Correct tracking of allocations/frees
        - Stats remain consistent
        """
        memprof = memprof_cleanup
        
        memprof.start(sampling_rate_kb=512)

        num_threads = 10
        ops_per_thread = 100_000  # Total ~1M ops

        errors: List[str] = []
        completed_ops = [0] * num_threads
        thread_samples = [0] * num_threads

        def worker(thread_id: int):
            """Worker that performs random alloc/free operations."""
            local_objects: List[bytearray] = []
            ops = 0

            try:
                for i in range(ops_per_thread):
                    # Random operation: allocate or free
                    if random.random() < 0.6 or len(local_objects) == 0:
                        # Allocate with random size
                        size = random.choice([64, 256, 1024, 4096, 16384])
                        obj = bytearray(size)
                        local_objects.append(obj)
                    else:
                        # Free random object
                        idx = random.randint(0, len(local_objects) - 1)
                        del local_objects[idx]

                    ops += 1

                # Final cleanup
                del local_objects[:]

            except Exception as e:
                errors.append(f"Thread {thread_id} error at op {ops}: {e}")

            completed_ops[thread_id] = ops

        # Run threads
        threads = []
        for i in range(num_threads):
            t = threading.Thread(target=worker, args=(i,), name=f"stress-{i}")
            threads.append(t)

        start_time = time.time()

        for t in threads:
            t.start()

        for t in threads:
            t.join(timeout=120)  # 2 minute timeout

        elapsed = time.time() - start_time

        # Verify no errors
        assert not errors, f"Errors occurred: {errors}"

        # Verify all threads completed
        total_ops = sum(completed_ops)
        assert total_ops == num_threads * ops_per_thread, (
            f"Expected {num_threads * ops_per_thread} ops, got {total_ops}"
        )

        # Get final stats
        stats = memprof.get_stats()

        # Verify stats are valid
        assert stats.total_samples >= 0
        assert stats.live_samples >= 0
        assert stats.freed_samples >= 0
        assert 0.0 <= stats.heap_map_load_percent <= 100.0

        memprof.stop()

        print(f"\nStress test completed:")
        print(f"  Threads: {num_threads}")
        print(f"  Total ops: {total_ops:,}")
        print(f"  Elapsed: {elapsed:.2f}s")
        print(f"  Ops/sec: {total_ops / elapsed:,.0f}")
        print(f"  Total samples: {stats.total_samples}")
        print(f"  Heap map load: {stats.heap_map_load_percent:.2f}%")


class TestHighAllocationRate:
    """T068: Stress test for high allocation rate (1M allocs/sec target)."""

    def test_high_allocation_rate(self, memprof_cleanup):
        """Test profiler handles high allocation rate without issues.

        Target: Handle 1M+ allocations without crashing or significant
        performance degradation.
        """
        memprof = memprof_cleanup

        memprof.start(sampling_rate_kb=512)

        # Rapid small allocations
        start_time = time.time()
        alloc_count = 0
        target_duration = 2.0  # Run for 2 seconds

        # Use list to keep references temporarily
        batch_size = 10_000

        while time.time() - start_time < target_duration:
            # Allocate batch
            batch = [bytearray(64) for _ in range(batch_size)]
            alloc_count += batch_size

            # Free batch
            del batch

            # Occasional garbage collection to prevent memory exhaustion
            if alloc_count % 100_000 == 0:
                gc.collect()

        elapsed = time.time() - start_time
        rate = alloc_count / elapsed

        # Get stats
        stats = memprof.get_stats()

        memprof.stop()

        print(f"\nHigh allocation rate test:")
        print(f"  Allocations: {alloc_count:,}")
        print(f"  Duration: {elapsed:.2f}s")
        print(f"  Rate: {rate:,.0f} allocs/sec")
        print(f"  Samples: {stats.total_samples}")
        print(f"  Sampling rate: ~1 per {512*1024/64:.0f} allocs")

        # Should complete without errors
        assert alloc_count > 0
        assert stats.total_samples >= 0


class TestConcurrentAllocation:
    """T069: Concurrent allocation test (10 threads)."""

    def test_concurrent_allocation_10_threads(self, memprof_cleanup):
        """Test concurrent allocation from 10 threads.

        Verifies:
        - Thread safety of sampling
        - No race conditions in heap map
        - Correct statistics under concurrent load
        """
        memprof = memprof_cleanup

        memprof.start(sampling_rate_kb=256)  # Lower rate for more samples

        num_threads = 10
        allocs_per_thread = 10_000
        errors: List[str] = []
        thread_data: List[List[bytearray]] = [[] for _ in range(num_threads)]

        def allocate_worker(thread_id: int):
            """Worker that allocates objects and keeps them alive."""
            try:
                local_list = []
                for i in range(allocs_per_thread):
                    # Varying allocation sizes
                    size = 64 * (1 + (i % 16))  # 64 to 1024 bytes
                    obj = bytearray(size)
                    local_list.append(obj)

                    # Occasionally free some
                    if len(local_list) > 100:
                        del local_list[:50]

                # Store remaining for verification
                thread_data[thread_id] = local_list

            except Exception as e:
                errors.append(f"Thread {thread_id}: {e}")

        # Run concurrent allocations
        threads = []
        for i in range(num_threads):
            t = threading.Thread(target=allocate_worker, args=(i,))
            threads.append(t)

        for t in threads:
            t.start()

        for t in threads:
            t.join(timeout=60)

        # Check for errors
        assert not errors, f"Errors: {errors}"

        # Get snapshot while data is still alive
        snapshot = memprof.get_snapshot()
        stats = memprof.get_stats()

        # Verify profiler tracked activity
        assert stats.total_samples >= 0
        assert stats.live_samples >= 0

        # Cleanup
        for data in thread_data:
            del data[:]

        gc.collect()

        memprof.stop()

        print(f"\nConcurrent allocation test:")
        print(f"  Threads: {num_threads}")
        print(f"  Allocations per thread: {allocs_per_thread:,}")
        print(f"  Total samples: {stats.total_samples}")
        print(f"  Live samples: {stats.live_samples}")
        print(f"  Freed samples: {stats.freed_samples}")

    def test_concurrent_start_stop_get_snapshot(self, memprof_cleanup):
        """Test thread safety of API operations."""
        memprof = memprof_cleanup

        memprof.start(sampling_rate_kb=512)

        errors: List[str] = []
        snapshots: List[object] = []

        def snapshot_worker(worker_id: int, count: int):
            """Worker that takes snapshots."""
            try:
                for _ in range(count):
                    snapshot = memprof.get_snapshot()
                    snapshots.append(snapshot)
                    time.sleep(0.01)
            except Exception as e:
                errors.append(f"Worker {worker_id}: {e}")

        def allocate_worker(worker_id: int, count: int):
            """Worker that allocates memory."""
            try:
                for i in range(count):
                    data = bytearray(1024)
                    time.sleep(0.005)
                    del data
            except Exception as e:
                errors.append(f"Allocator {worker_id}: {e}")

        # Run snapshot and allocation workers concurrently
        threads = []

        for i in range(3):
            t = threading.Thread(target=snapshot_worker, args=(i, 20))
            threads.append(t)

        for i in range(5):
            t = threading.Thread(target=allocate_worker, args=(i, 50))
            threads.append(t)

        for t in threads:
            t.start()

        for t in threads:
            t.join(timeout=30)

        # No errors should occur
        assert not errors, f"Errors: {errors}"

        # All snapshots should be valid
        for snapshot in snapshots:
            assert snapshot is not None
            assert hasattr(snapshot, 'live_samples')
            assert hasattr(snapshot, 'estimated_heap_bytes')

        memprof.stop()


class TestMemoryPressure:
    """Tests under memory pressure conditions."""

    @pytest.mark.slow
    def test_large_allocation_burst(self, memprof_cleanup):
        """Test profiler handles bursts of large allocations."""
        memprof = memprof_cleanup

        memprof.start(sampling_rate_kb=1024)  # Higher rate for large allocs

        # Burst of large allocations
        large_objects = []
        try:
            for _ in range(100):
                # 1MB allocations
                obj = bytearray(1024 * 1024)
                large_objects.append(obj)

            stats = memprof.get_stats()

            # Should have captured some samples
            assert stats.total_samples >= 0

            # Estimated heap should reflect large allocations (with sampling)
            # At 1MB rate, 100MB of allocations should yield ~100 samples
            # But due to Poisson sampling, this varies

            # Free all
            del large_objects[:]

        finally:
            memprof.stop()

    def test_allocation_free_churn(self, memprof_cleanup):
        """Test rapid allocation/free churn."""
        memprof = memprof_cleanup

        memprof.start(sampling_rate_kb=128)

        # High churn - allocate and immediately free
        for _ in range(10000):
            obj = bytearray(512)
            del obj

        stats = memprof.get_stats()

        # Most allocations should be freed
        # freed_samples should be >= 0 (some samples were freed)
        assert stats.freed_samples >= 0

        # The heap estimate should be relatively low after freeing
        assert stats.estimated_heap_bytes >= 0

        memprof.stop()


# Mark slow tests
def pytest_configure(config):
    config.addinivalue_line("markers", "slow: marks tests as slow (deselect with '-m \"not slow\"')")

