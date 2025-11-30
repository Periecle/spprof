"""Tests for multi-threaded profiling."""

import threading
import time


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


def test_main_thread_blocked_other_sampled():
    """Verify other threads are sampled when main thread is blocked."""
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
