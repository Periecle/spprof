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


# ============================================================================
# Memory Profiler Benchmarks (T119, T120)
# ============================================================================

def memprof_overhead_benchmark():
    """Benchmark memory profiler overhead at various sampling rates.

    Task T119: Performance benchmark at various sampling rates
    """
    import spprof.memprof as memprof

    print("\n" + "=" * 70)
    print("Memory Profiler Overhead Benchmark")
    print("=" * 70)

    def workload():
        """Mixed CPU/memory workload."""
        result = 0
        for i in range(100000):
            result += i ** 2
            if i % 100 == 0:
                data = bytearray(1024)
                del data
        return result

    # Baseline without profiler
    gc.collect()
    times = []
    for _ in range(5):
        start = time.perf_counter()
        workload()
        times.append(time.perf_counter() - start)
    baseline_time = sum(times) / len(times)
    print(f"\nBaseline (no profiler): {baseline_time*1000:.2f} ms")

    # Test various sampling rates
    rates = [64, 128, 256, 512, 1024]
    results = []

    for rate_kb in rates:
        gc.collect()

        # Reset module state
        memprof._initialized = False
        memprof._running = False
        memprof._shutdown = False

        times = []
        for _ in range(5):
            memprof.start(sampling_rate_kb=rate_kb)
            start = time.perf_counter()
            workload()
            elapsed = time.perf_counter() - start
            stats = memprof.get_stats()
            memprof.stop()
            memprof.shutdown()
            memprof._initialized = False
            memprof._running = False
            memprof._shutdown = False
            times.append(elapsed)

        avg_time = sum(times) / len(times)
        overhead = (avg_time - baseline_time) / baseline_time * 100

        results.append({
            "rate_kb": rate_kb,
            "avg_time_ms": avg_time * 1000,
            "overhead_pct": overhead,
            "samples": stats.total_samples if stats else 0,
        })

        print(f"  {rate_kb:4d} KB rate: {avg_time*1000:.2f} ms "
              f"(overhead: {overhead:.3f}%, samples: {stats.total_samples if stats else 0})")

    print("\nResults:")
    print("-" * 50)
    print(f"{'Rate (KB)':>10} {'Time (ms)':>12} {'Overhead %':>12} {'Samples':>10}")
    print("-" * 50)
    for r in results:
        print(f"{r['rate_kb']:>10} {r['avg_time_ms']:>12.2f} "
              f"{r['overhead_pct']:>12.3f} {r['samples']:>10}")

    # Check target
    target_rate = 512
    for r in results:
        if r['rate_kb'] == target_rate:
            if r['overhead_pct'] < 0.1:
                print(f"\n✓ Target overhead (<0.1% at {target_rate}KB) ACHIEVED: {r['overhead_pct']:.3f}%")
            elif r['overhead_pct'] < 1.0:
                print(f"\n⚠ Target overhead (<0.1% at {target_rate}KB) not met: {r['overhead_pct']:.3f}%")
            else:
                print(f"\n✗ High overhead at {target_rate}KB: {r['overhead_pct']:.2f}%")

    return results


def memprof_footprint_benchmark():
    """Verify memory profiler footprint stays under 60MB.

    Task T120: Memory footprint verification (<60MB)
    """
    import resource
    import spprof.memprof as memprof

    print("\n" + "=" * 70)
    print("Memory Profiler Footprint Benchmark")
    print("=" * 70)

    def get_rss_mb():
        """Get resident set size in MB."""
        usage = resource.getrusage(resource.RUSAGE_SELF)
        return usage.ru_maxrss / 1024  # ru_maxrss is in KB on Linux, bytes on macOS
        # Note: On macOS, divide by 1024*1024 instead

    # Baseline memory
    gc.collect()
    baseline_rss = get_rss_mb()
    print(f"\nBaseline RSS: {baseline_rss:.2f} MB")

    # Reset module state
    memprof._initialized = False
    memprof._running = False
    memprof._shutdown = False

    # Initialize profiler
    memprof.start(sampling_rate_kb=64)

    # Measure after initialization
    gc.collect()
    init_rss = get_rss_mb()
    print(f"After init RSS: {init_rss:.2f} MB")
    print(f"Init overhead: {init_rss - baseline_rss:.2f} MB")

    # Do lots of allocations to exercise data structures
    print("\nRunning workload with many allocations...")
    objects = []
    for i in range(10000):
        obj = bytearray(512)
        objects.append(obj)
        if i % 2 == 0:
            del objects[i // 2]
            objects[i // 2] = None

    # Measure after workload
    gc.collect()
    workload_rss = get_rss_mb()
    stats = memprof.get_stats()

    print(f"After workload RSS: {workload_rss:.2f} MB")
    print(f"Total overhead: {workload_rss - baseline_rss:.2f} MB")
    print(f"Samples: {stats.total_samples}")
    print(f"Heap map load: {stats.heap_map_load_percent:.2f}%")

    memprof.stop()
    memprof.shutdown()

    # Theoretical max footprint:
    # - Heap map: 1M entries × 24 bytes = 24 MB
    # - Stack table: 64K entries × 544 bytes = 35 MB
    # - Bloom filter: 128 KB
    # - Total: ~60 MB max
    theoretical_max = 60

    print(f"\nTheoretical max footprint: {theoretical_max} MB")
    actual_overhead = workload_rss - baseline_rss

    if actual_overhead < theoretical_max:
        print(f"✓ Memory footprint OK: {actual_overhead:.2f} MB < {theoretical_max} MB")
    else:
        print(f"⚠ Memory footprint high: {actual_overhead:.2f} MB >= {theoretical_max} MB")

    return {
        "baseline_mb": baseline_rss,
        "init_mb": init_rss,
        "workload_mb": workload_rss,
        "overhead_mb": actual_overhead,
        "target_mb": theoretical_max,
        "passed": actual_overhead < theoretical_max,
    }


def run_memprof_benchmarks():
    """Run all memory profiler benchmarks."""
    print("=" * 70)
    print("Memory Profiler Benchmarks")
    print("=" * 70)

    try:
        overhead_results = memprof_overhead_benchmark()
    except Exception as e:
        print(f"Overhead benchmark failed: {e}")
        overhead_results = None

    try:
        footprint_results = memprof_footprint_benchmark()
    except Exception as e:
        print(f"Footprint benchmark failed: {e}")
        footprint_results = None

    print("\n" + "=" * 70)
    print("Summary")
    print("=" * 70)

    if overhead_results:
        for r in overhead_results:
            if r['rate_kb'] == 512:
                print(f"Overhead at 512KB: {r['overhead_pct']:.3f}%")

    if footprint_results:
        print(f"Memory footprint: {footprint_results['overhead_mb']:.2f} MB "
              f"({'OK' if footprint_results['passed'] else 'HIGH'})")
