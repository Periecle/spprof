#!/usr/bin/env python3
"""
Basic profiling example demonstrating the start/stop/save workflow.

Usage:
    python examples/basic_profile.py

This will generate a profile.json file that can be opened in Speedscope:
    https://www.speedscope.app
"""

import spprof


def fibonacci(n: int) -> int:
    """Calculate nth Fibonacci number (inefficient recursive version)."""
    if n <= 1:
        return n
    return fibonacci(n - 1) + fibonacci(n - 2)


def cpu_intensive_work():
    """Do some CPU-intensive computations."""
    print("Computing Fibonacci numbers...")

    results = []
    for i in range(30):
        result = fibonacci(i)
        results.append(result)

    print(f"Computed {len(results)} Fibonacci numbers")
    return results


def main():
    print("Starting profiler with 10ms sampling interval...")

    # Start profiling
    spprof.start(interval_ms=10)

    # Run workload
    _results = cpu_intensive_work()

    # Stop and get profile
    profile = spprof.stop()

    print("\nProfile statistics:")
    print(f"  Samples collected: {len(profile.samples)}")
    print(f"  Samples dropped: {profile.dropped_count}")
    print(f"  Duration: {(profile.end_time - profile.start_time).total_seconds():.3f}s")
    print(f"  Python version: {profile.python_version}")
    print(f"  Platform: {profile.platform}")

    # Save to file
    output_path = "profile.json"
    profile.save(output_path)
    print(f"\nProfile saved to: {output_path}")
    print("Open in Speedscope: https://www.speedscope.app")


if __name__ == "__main__":
    main()
