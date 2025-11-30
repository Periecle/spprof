#!/usr/bin/env python3
"""
CPU overhead measurement for spprof.

Measures the overhead of profiling at various sampling intervals.
"""

from __future__ import annotations

import argparse
import time
from typing import Callable


def cpu_bound_workload(iterations: int = 1_000_000) -> int:
    """CPU-intensive workload for benchmarking."""
    total = 0
    for i in range(iterations):
        total += i * i
    return total


def measure_time(func: Callable[[], None], iterations: int = 5) -> float:
    """Measure average execution time of a function."""
    times = []
    for _ in range(iterations):
        start = time.perf_counter()
        func()
        end = time.perf_counter()
        times.append(end - start)

    # Remove outliers and average
    times.sort()
    if len(times) > 2:
        times = times[1:-1]  # Remove min and max

    return sum(times) / len(times)


def measure_overhead(interval_ms: int, workload_iterations: int = 1_000_000) -> dict:
    """Measure profiling overhead at a given interval."""
    import spprof

    def workload():
        cpu_bound_workload(workload_iterations)

    # Baseline (no profiling)
    baseline_time = measure_time(workload)

    # With profiling
    def profiled_workload():
        spprof.start(interval_ms=interval_ms)
        cpu_bound_workload(workload_iterations)
        spprof.stop()

    profiled_time = measure_time(profiled_workload)

    # Calculate overhead
    overhead = profiled_time - baseline_time
    overhead_pct = (overhead / baseline_time) * 100

    return {
        "interval_ms": interval_ms,
        "baseline_time": baseline_time,
        "profiled_time": profiled_time,
        "overhead": overhead,
        "overhead_pct": overhead_pct,
    }


def main():
    parser = argparse.ArgumentParser(description="Measure spprof overhead")
    parser.add_argument(
        "--intervals",
        type=int,
        nargs="+",
        default=[1, 10, 100],
        help="Sampling intervals to test (ms)",
    )
    parser.add_argument(
        "--iterations",
        type=int,
        default=1_000_000,
        help="Workload iterations",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="Output as JSON",
    )

    args = parser.parse_args()

    results = []

    print("Measuring spprof overhead...")
    print(f"Workload: {args.iterations} iterations of CPU-bound work")
    print()

    for interval in args.intervals:
        print(f"Testing {interval}ms interval...", end=" ", flush=True)
        result = measure_overhead(interval, args.iterations)
        results.append(result)
        print(f"Overhead: {result['overhead_pct']:.2f}%")

    print()
    print("Results:")
    print("-" * 60)
    print(f"{'Interval':>10} {'Baseline':>12} {'Profiled':>12} {'Overhead':>10}")
    print("-" * 60)

    for r in results:
        print(
            f"{r['interval_ms']:>8}ms {r['baseline_time']:>10.3f}s "
            f"{r['profiled_time']:>10.3f}s {r['overhead_pct']:>8.2f}%"
        )

    if args.json:
        import json

        print()
        print("JSON output:")
        print(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
