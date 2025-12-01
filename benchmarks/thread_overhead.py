#!/usr/bin/env python3
"""
Multi-threading overhead benchmark for spprof.

Measures the impact of profiling on multi-threaded workloads.
"""

from __future__ import annotations

import argparse
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from typing import Any


def cpu_bound_task(iterations: int) -> int:
    """CPU-intensive task for benchmarking."""
    total = 0
    for i in range(iterations):
        total += i * i
    return total


def io_bound_task(sleep_ms: int, iterations: int) -> int:
    """I/O-bound task (simulated with sleep)."""
    total = 0
    for i in range(iterations):
        time.sleep(sleep_ms / 1000.0)
        total += i
    return total


def measure_threading_overhead(
    num_threads: int,
    iterations_per_thread: int,
    interval_ms: int,
    with_registration: bool = True,
) -> dict[str, Any]:
    """Measure overhead of profiling multi-threaded workloads."""
    import spprof

    results = []
    barrier = threading.Barrier(num_threads + 1)

    def worker():
        if with_registration:
            spprof.register_thread()
        try:
            barrier.wait()  # Sync start
            result = cpu_bound_task(iterations_per_thread)
            results.append(result)
        finally:
            if with_registration:
                spprof.unregister_thread()

    # Baseline (no profiling)
    results.clear()
    threads = [
        threading.Thread(target=lambda: results.append(cpu_bound_task(iterations_per_thread)))
        for _ in range(num_threads)
    ]

    start = time.perf_counter()
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    baseline_time = time.perf_counter() - start

    # With profiling
    results.clear()
    spprof.start(interval_ms=interval_ms)

    threads = [threading.Thread(target=worker) for _ in range(num_threads)]
    for t in threads:
        t.start()

    barrier.wait()  # Let all workers start

    start = time.perf_counter()
    for t in threads:
        t.join()
    profiled_time = time.perf_counter() - start

    profile = spprof.stop()

    overhead = profiled_time - baseline_time
    overhead_pct = (overhead / baseline_time) * 100 if baseline_time > 0 else 0

    # Count samples per thread
    thread_samples: dict[int, int] = {}
    for sample in profile.samples:
        tid = sample.thread_id
        thread_samples[tid] = thread_samples.get(tid, 0) + 1

    return {
        "num_threads": num_threads,
        "interval_ms": interval_ms,
        "with_registration": with_registration,
        "baseline_time": baseline_time,
        "profiled_time": profiled_time,
        "overhead": overhead,
        "overhead_pct": overhead_pct,
        "total_samples": len(profile.samples),
        "threads_with_samples": len(thread_samples),
        "samples_per_thread": list(thread_samples.values()),
    }


def measure_threadpool_overhead(
    num_workers: int,
    num_tasks: int,
    iterations_per_task: int,
    interval_ms: int,
) -> dict[str, Any]:
    """Measure overhead with ThreadPoolExecutor."""
    import spprof

    def task():
        return cpu_bound_task(iterations_per_task)

    # Baseline
    with ThreadPoolExecutor(max_workers=num_workers) as executor:
        start = time.perf_counter()
        list(executor.map(lambda _: task(), range(num_tasks)))
        baseline_time = time.perf_counter() - start

    # With profiling
    spprof.start(interval_ms=interval_ms)

    with ThreadPoolExecutor(max_workers=num_workers) as executor:
        start = time.perf_counter()
        list(executor.map(lambda _: task(), range(num_tasks)))
        profiled_time = time.perf_counter() - start

    profile = spprof.stop()

    overhead = profiled_time - baseline_time
    overhead_pct = (overhead / baseline_time) * 100 if baseline_time > 0 else 0

    return {
        "num_workers": num_workers,
        "num_tasks": num_tasks,
        "interval_ms": interval_ms,
        "baseline_time": baseline_time,
        "profiled_time": profiled_time,
        "overhead_pct": overhead_pct,
        "total_samples": len(profile.samples),
    }


def main():
    parser = argparse.ArgumentParser(description="Measure spprof threading overhead")
    parser.add_argument(
        "--threads",
        type=int,
        nargs="+",
        default=[2, 4, 8],
        help="Number of threads to test",
    )
    parser.add_argument(
        "--interval",
        type=int,
        default=10,
        help="Sampling interval (ms)",
    )
    parser.add_argument(
        "--iterations",
        type=int,
        default=500000,
        help="Iterations per thread",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="Output as JSON",
    )

    args = parser.parse_args()

    print("Measuring spprof threading overhead...")
    print(f"Python version: {sys.version}")
    print(f"Sampling interval: {args.interval}ms")
    print()

    results = []

    print("Direct threading test:")
    print("-" * 80)

    for num_threads in args.threads:
        print(f"Testing {num_threads} threads...", end=" ", flush=True)
        result = measure_threading_overhead(
            num_threads=num_threads,
            iterations_per_thread=args.iterations,
            interval_ms=args.interval,
        )
        results.append(result)
        print(
            f"Overhead: {result['overhead_pct']:.2f}%, "
            f"Samples: {result['total_samples']}, "
            f"Threads sampled: {result['threads_with_samples']}"
        )

    print()
    print("ThreadPoolExecutor test:")
    print("-" * 80)

    for num_workers in args.threads:
        print(f"Testing {num_workers} workers, {num_workers * 2} tasks...", end=" ", flush=True)
        result = measure_threadpool_overhead(
            num_workers=num_workers,
            num_tasks=num_workers * 2,
            iterations_per_task=args.iterations // 2,
            interval_ms=args.interval,
        )
        results.append(result)
        print(f"Overhead: {result['overhead_pct']:.2f}%, Samples: {result['total_samples']}")

    # Summary
    print()
    print("Summary:")
    print("-" * 80)

    max_overhead = max(r.get("overhead_pct", 0) for r in results)
    if max_overhead < 5:
        print(f"✓ Threading overhead acceptable: {max_overhead:.2f}% max")
    elif max_overhead < 10:
        print(f"⚠ Threading overhead moderate: {max_overhead:.2f}% max")
    else:
        print(f"✗ Threading overhead high: {max_overhead:.2f}% max")

    if args.json:
        import json

        print()
        print("JSON output:")
        print(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
