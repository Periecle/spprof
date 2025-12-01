#!/usr/bin/env python3
"""
Run all spprof benchmarks and generate a report.

Usage:
    python benchmarks/run_all.py
    python benchmarks/run_all.py --output results.json
"""

from __future__ import annotations

import argparse
import json
import platform
import sys
import time
from datetime import datetime
from pathlib import Path


def get_system_info() -> dict:
    """Collect system information."""
    return {
        "python_version": sys.version,
        "platform": platform.platform(),
        "processor": platform.processor(),
        "cpu_count": platform.os.cpu_count(),  # type: ignore
        "timestamp": datetime.now().isoformat(),
    }


def run_overhead_benchmark() -> dict:
    """Run CPU overhead benchmark."""
    print("\n" + "=" * 60)
    print("CPU Overhead Benchmark")
    print("=" * 60)

    from benchmarks.overhead import measure_overhead

    results = []
    for interval in [1, 10, 100]:
        print(f"  Testing {interval}ms interval...", end=" ", flush=True)
        result = measure_overhead(interval, workload_iterations=500_000)
        results.append(result)
        print(f"Overhead: {result['overhead_pct']:.2f}%")

    return {"overhead": results}


def run_memory_benchmark() -> dict:
    """Run memory usage benchmark."""
    print("\n" + "=" * 60)
    print("Memory Usage Benchmark")
    print("=" * 60)

    from benchmarks.memory import measure_memory_usage

    results = []
    for interval, duration in [(10, 1.0), (10, 5.0), (1, 1.0)]:
        print(f"  Testing {interval}ms interval, {duration}s duration...", end=" ", flush=True)
        result = measure_memory_usage(interval, duration)
        results.append(result)
        print(f"Peak: {result['peak_mb']:.2f} MB")

    return {"memory": results}


def run_threading_benchmark() -> dict:
    """Run threading overhead benchmark."""
    print("\n" + "=" * 60)
    print("Threading Overhead Benchmark")
    print("=" * 60)

    from benchmarks.thread_overhead import measure_threading_overhead

    results = []
    for num_threads in [2, 4, 8]:
        print(f"  Testing {num_threads} threads...", end=" ", flush=True)
        result = measure_threading_overhead(
            num_threads=num_threads,
            iterations_per_thread=200_000,
            interval_ms=10,
        )
        results.append(result)
        print(f"Overhead: {result['overhead_pct']:.2f}%")

    return {"threading": results}


def check_targets(results: dict) -> dict:
    """Check if benchmarks meet target thresholds."""
    checks = {}

    # CPU overhead target: <5% at 10ms interval
    overhead_results = results.get("overhead", [])
    for r in overhead_results:
        if r.get("interval_ms") == 10:
            checks["cpu_overhead_10ms"] = {
                "target": "<5%",
                "actual": f"{r['overhead_pct']:.2f}%",
                "passed": r["overhead_pct"] < 5,
            }
            break

    # Memory target: <100MB peak
    memory_results = results.get("memory", [])
    if memory_results:
        max_peak = max(r.get("peak_mb", 0) for r in memory_results)
        checks["memory_peak"] = {
            "target": "<100 MB",
            "actual": f"{max_peak:.2f} MB",
            "passed": max_peak < 100,
        }

    # Threading target: <10% overhead
    threading_results = results.get("threading", [])
    if threading_results:
        max_overhead = max(r.get("overhead_pct", 0) for r in threading_results)
        checks["threading_overhead"] = {
            "target": "<10%",
            "actual": f"{max_overhead:.2f}%",
            "passed": max_overhead < 10,
        }

    return checks


def main():
    parser = argparse.ArgumentParser(description="Run all spprof benchmarks")
    parser.add_argument(
        "--output",
        type=Path,
        help="Output file for JSON results",
    )
    parser.add_argument(
        "--quick",
        action="store_true",
        help="Run quick benchmarks only",
    )

    args = parser.parse_args()

    print("=" * 60)
    print("spprof Benchmark Suite")
    print("=" * 60)
    print(f"Python: {sys.version}")
    print(f"Platform: {platform.platform()}")

    start_time = time.perf_counter()
    all_results = {"system": get_system_info()}

    try:
        all_results.update(run_overhead_benchmark())
    except Exception as e:
        print(f"  ✗ Overhead benchmark failed: {e}")
        all_results["overhead_error"] = str(e)

    if not args.quick:
        try:
            all_results.update(run_memory_benchmark())
        except Exception as e:
            print(f"  ✗ Memory benchmark failed: {e}")
            all_results["memory_error"] = str(e)

        try:
            all_results.update(run_threading_benchmark())
        except Exception as e:
            print(f"  ✗ Threading benchmark failed: {e}")
            all_results["threading_error"] = str(e)

    elapsed = time.perf_counter() - start_time

    # Check targets
    checks = check_targets(all_results)
    all_results["checks"] = checks

    # Summary
    print("\n" + "=" * 60)
    print("Summary")
    print("=" * 60)
    print(f"Total time: {elapsed:.1f}s")
    print()

    all_passed = True
    for name, check in checks.items():
        status = "✓" if check["passed"] else "✗"
        all_passed = all_passed and check["passed"]
        print(f"  {status} {name}: {check['actual']} (target: {check['target']})")

    print()
    if all_passed:
        print("All benchmarks PASSED ✓")
    else:
        print("Some benchmarks FAILED ✗")

    # Save results
    if args.output:
        args.output.write_text(json.dumps(all_results, indent=2))
        print(f"\nResults saved to: {args.output}")

    return 0 if all_passed else 1


if __name__ == "__main__":
    sys.exit(main())
