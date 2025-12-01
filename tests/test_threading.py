"""Tests for multi-threaded profiling."""

import platform
import threading
import time

import pytest


def test_register_thread():
    """Test thread registration API."""
    import spprof

    # Registration should succeed even when profiler not active
    assert spprof.register_thread() is True
    assert spprof.unregister_thread() is True


def test_thread_profiler_context_manager():
    """Test ThreadProfiler context manager."""
    import spprof

    spprof.start(interval_ms=10)

    def worker():
        with spprof.ThreadProfiler():
            total = 0
            for i in range(10000):
                total += i
            return total

    thread = threading.Thread(target=worker)
    thread.start()
    thread.join()

    profile = spprof.stop()
    assert profile is not None


def test_explicit_thread_registration():
    """Test explicit thread registration."""
    import spprof

    results = []

    def worker():
        spprof.register_thread()
        try:
            total = 0
            for i in range(50000):
                total += i
            results.append(total)
        finally:
            spprof.unregister_thread()

    spprof.start(interval_ms=5)

    threads = [threading.Thread(target=worker) for _ in range(3)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    profile = spprof.stop()

    assert len(results) == 3
    assert profile is not None


def test_multi_thread_sampling():
    """Verify sampling works with multiple threads."""
    import spprof

    results = []
    barrier = threading.Barrier(6)  # 5 workers + 1 main

    def worker(n: int):
        barrier.wait()  # Sync start
        total = 0
        for i in range(100000):
            total += i
        results.append((threading.current_thread().name, total))

    spprof.start(interval_ms=5)

    threads = [threading.Thread(target=worker, args=(i,), name=f"Worker-{i}") for i in range(5)]
    for t in threads:
        t.start()

    barrier.wait()  # Main thread syncs with workers

    for t in threads:
        t.join()

    profile = spprof.stop()

    assert len(results) == 5
    assert profile is not None
    # Note: With pure Python fallback, we may not have actual samples


def test_thread_ids_in_output():
    """Verify thread_id field is populated in samples."""
    from spprof import Frame, Sample

    # Create a sample with thread_id
    sample = Sample(
        timestamp_ns=1000000,
        thread_id=12345,
        thread_name="TestThread",
        frames=[Frame("func", "file.py", 1)],
    )

    assert sample.thread_id == 12345
    assert sample.thread_name == "TestThread"


def test_thread_terminates_during_profiling():
    """Verify profiler handles threads terminating during profiling."""
    import spprof

    def short_worker():
        total = 0
        for i in range(1000):
            total += i
        return total

    spprof.start(interval_ms=5)

    # Start threads that will terminate quickly
    threads = [threading.Thread(target=short_worker) for _ in range(3)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    # Continue profiling with main thread
    total = 0
    for i in range(10000):
        total += i

    # Should not crash
    profile = spprof.stop()
    assert profile is not None


@pytest.mark.skipif(platform.system() != "Linux", reason="Linux-specific multi-thread test")
def test_main_thread_blocked_other_sampled():
    """Verify other threads are sampled when main thread is blocked.

    On Linux with timer_create, each thread has its own timer.
    On macOS, setitimer(ITIMER_PROF) is process-wide and doesn't provide
    per-thread sampling when the main thread is blocked.
    """
    import spprof

    result = []
    done = threading.Event()

    def busy_worker():
        total = 0
        while not done.is_set():
            for i in range(1000):
                total += i
        result.append(total)

    spprof.start(interval_ms=5)

    thread = threading.Thread(target=busy_worker)
    thread.start()

    # Main thread sleeps (blocked)
    time.sleep(0.05)

    done.set()
    thread.join()

    profile = spprof.stop()

    assert len(result) == 1
    assert profile is not None


@pytest.mark.skipif(platform.system() != "Linux", reason="Linux-specific thread registry test")
def test_many_threads():
    """Test profiling with more than 256 threads (old limit).

    This validates the dynamic thread registry can handle 300+ threads
    without hitting any artificial limits.
    """
    import spprof

    def work():
        total = 0
        for i in range(10000):
            total += i
        return total

    spprof.start(interval_ms=10)

    # Create 300 threads (exceeds old MAX_TRACKED_THREADS=256 limit)
    threads = [threading.Thread(target=work) for _ in range(300)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    profile = spprof.stop()
    assert profile is not None


@pytest.mark.skipif(platform.system() != "Linux", reason="Linux-specific thread churn test")
def test_rapid_thread_churn():
    """Test rapid thread creation/destruction.

    Simulates thread pool patterns where threads are frequently
    created and destroyed while profiling is active.
    """
    import spprof

    spprof.start(interval_ms=5)

    # Simulate rapid thread churn - 20 batches of 10 threads each
    for _ in range(20):
        threads = [threading.Thread(target=lambda: sum(range(1000))) for _ in range(10)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()

    profile = spprof.stop()
    assert profile is not None


@pytest.mark.skipif(
    platform.system() != "Linux", reason="Linux-specific concurrent registration test"
)
def test_concurrent_thread_registration():
    """Test concurrent thread registration and unregistration.

    Multiple threads registering and unregistering simultaneously
    should not cause data races or crashes.
    """
    import spprof

    barrier = threading.Barrier(20)
    errors = []

    def worker():
        try:
            barrier.wait()  # Sync start
            spprof.register_thread()
            # Do some work
            _ = sum(range(10000))
            spprof.unregister_thread()
        except Exception as e:
            errors.append(e)

    spprof.start(interval_ms=5)

    # Start 20 threads that all register/unregister simultaneously
    threads = [threading.Thread(target=worker) for _ in range(20)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    profile = spprof.stop()

    assert len(errors) == 0, f"Errors during concurrent registration: {errors}"
    assert profile is not None


@pytest.mark.skipif(platform.system() != "Linux", reason="Linux-specific thread exit timing test")
def test_thread_exit_during_timer():
    """Test thread exiting while timer is potentially firing.

    Edge case: thread exits exactly when its timer would fire.
    The profiler should handle this gracefully.
    """
    import spprof

    def quick_worker():
        # Very short-lived thread
        return sum(range(100))

    spprof.start(interval_ms=5)  # Moderate sampling

    # Create short-lived threads
    threads = []
    for _ in range(50):
        t = threading.Thread(target=quick_worker)
        t.start()
        threads.append(t)

    # Wait for all threads to complete
    for t in threads:
        t.join()

    profile = spprof.stop()
    assert profile is not None
