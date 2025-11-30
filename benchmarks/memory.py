#!/usr/bin/env python3
"""
Memory usage benchmark for spprof.

Measures memory consumption during profiling at various sample rates
and profile durations.
"""

from __future__ import annotations

import argparse
import gc
import sys
import time
import tracemalloc
from typing import Any


def get_memory_usage() -> int:
    """Get current memory usage in bytes."""
    tracemalloc.start()
    gc.collect()
    current, _peak = tracemalloc.get_traced_memory()
    tracemalloc.stop()
    return current


def cpu_bound_workload(duration_sec: float) -> int:
    """CPU-intensive workload that runs for approximately duration_sec."""
    start = time.perf_counter()
    total = 0
    i = 0
    while time.perf_counter() - start < duration_sec:
        total += i * i
        i += 1
    return total


def measure_memory_usage(
    interval_ms: int,
    duration_sec: float,
) -> dict[str, Any]:
    """Measure memory usage during profiling."""
    import spprof

    # Measure baseline memory
    gc.collect()
    tracemalloc.start()
    baseline = tracemalloc.get_traced_memory()[0]

    # Start profiling
    spprof.start(interval_ms=interval_ms)

    # Run workload
    cpu_bound_workload(duration_sec)

    # Measure during profiling
    during = tracemalloc.get_traced_memory()[0]

    # Stop and get profile
    profile = spprof.stop()

    # Measure after profiling
    after = tracemalloc.get_traced_memory()[0]
    peak = tracemalloc.get_traced_memory()[1]

    tracemalloc.stop()

    return {
        "interval_ms": interval_ms,
        "duration_sec": duration_sec,
        "sample_count": len(profile.samples),
        "baseline_mb": baseline / (1024 * 1024),
        "during_mb": during / (1024 * 1024),
        "after_mb": after / (1024 * 1024),
        "peak_mb": peak / (1024 * 1024),
        "profile_memory_mb": (after - baseline) / (1024 * 1024),
    }


def main():
    parser = argparse.ArgumentParser(description="Measure spprof memory usage")
    parser.add_argument(
        "--intervals",
        type=int,
        nargs="+",
        default=[1, 10, 100],
        help="Sampling intervals to test (ms)",
    )
    parser.add_argument(
        "--durations",
        type=float,
        nargs="+",
        default=[1.0, 5.0, 10.0],
        help="Profile durations to test (seconds)",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="Output as JSON",
    )

    args = parser.parse_args()

    results = []

    print("Measuring spprof memory usage...")
    print(f"Python version: {sys.version}")
    print()

    for interval in args.intervals:
        for duration in args.durations:
            print(f"Testing {interval}ms interval, {duration}s duration...", end=" ", flush=True)

            # Run multiple times and average
            measurements = []
            for _ in range(3):
                gc.collect()
                result = measure_memory_usage(interval, duration)
                measurements.append(result)

            # Average the results
            avg_result = {
                "interval_ms": interval,
                "duration_sec": duration,
                "sample_count": sum(m["sample_count"] for m in measurements) // len(measurements),
                "peak_mb": sum(m["peak_mb"] for m in measurements) / len(measurements),
                "profile_memory_mb": sum(m["profile_memory_mb"] for m in measurements)
                / len(measurements),
            }

            results.append(avg_result)
            print(f"{avg_result['sample_count']} samples, {avg_result['peak_mb']:.2f} MB peak")

    print()
    print("Results:")
    print("-" * 70)
    print(f"{'Interval':>10} {'Duration':>10} {'Samples':>10} {'Peak MB':>10} {'Profile MB':>12}")
    print("-" * 70)

    for r in results:
        print(
            f"{r['interval_ms']:>8}ms {r['duration_sec']:>8.1f}s "
            f"{r['sample_count']:>10} {r['peak_mb']:>10.2f} {r['profile_memory_mb']:>12.2f}"
        )

    # Check if memory stays within reasonable bounds
    print()
    max_peak = max(r["peak_mb"] for r in results)
    if max_peak < 100:
        print(f"✓ Memory usage acceptable: {max_peak:.2f} MB peak")
    else:
        print(f"⚠ High memory usage: {max_peak:.2f} MB peak")

    if args.json:
        import json

        print()
        print("JSON output:")
        print(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
