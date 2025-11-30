#!/usr/bin/env python3
"""
Simple profiling example - demonstrates the API without signal-based sampling.

This works around the async-signal-safety issues by using the pure Python
fallback mode, then manually creating sample data for demonstration.
"""

import math
import sys
from datetime import datetime
from pathlib import Path


# Add src to path for development
sys.path.insert(0, str(Path(__file__).parent.parent / "src"))

import spprof
from spprof import Frame, Profile, Sample


def pure_python_fibonacci(n: int) -> int:
    """Pure Python recursive Fibonacci."""
    if n <= 1:
        return n
    return pure_python_fibonacci(n - 1) + pure_python_fibonacci(n - 2)


def pure_python_work():
    """Pure Python CPU-intensive work."""
    print("  Running pure Python work...")
    results = []
    for i in range(25):
        results.append(pure_python_fibonacci(i))
    return len(results)


def native_math_work():
    """Work using C math library functions."""
    print("  Running native math operations...")
    result = 0.0
    for i in range(1, 50000):
        x = float(i)
        result += math.sin(x) * math.cos(x)
    return result


def mixed_workload():
    """Run a mixed workload."""
    print("\nRunning mixed workload...")
    results = {}
    results["fibonacci"] = pure_python_work()
    results["math"] = native_math_work()
    return results


def create_demo_profile():
    """Create a demonstration profile with sample data."""
    # Simulate what the profiler would capture
    samples = []

    # Sample 1: In pure_python_fibonacci
    samples.append(
        Sample(
            timestamp_ns=1000000000,
            thread_id=1,
            thread_name="MainThread",
            frames=[
                Frame("pure_python_fibonacci", "examples/simple_profile.py", 25),
                Frame("pure_python_fibonacci", "examples/simple_profile.py", 28),
                Frame("pure_python_fibonacci", "examples/simple_profile.py", 28),
                Frame("pure_python_work", "examples/simple_profile.py", 33),
                Frame("mixed_workload", "examples/simple_profile.py", 48),
                Frame("main", "examples/simple_profile.py", 100),
            ],
        )
    )

    # Sample 2: Also in fibonacci (different depth)
    samples.append(
        Sample(
            timestamp_ns=1001000000,
            thread_id=1,
            thread_name="MainThread",
            frames=[
                Frame("pure_python_fibonacci", "examples/simple_profile.py", 25),
                Frame("pure_python_fibonacci", "examples/simple_profile.py", 28),
                Frame("pure_python_work", "examples/simple_profile.py", 33),
                Frame("mixed_workload", "examples/simple_profile.py", 48),
                Frame("main", "examples/simple_profile.py", 100),
            ],
        )
    )

    # Sample 3: In native math work with native frame
    samples.append(
        Sample(
            timestamp_ns=1002000000,
            thread_id=1,
            thread_name="MainThread",
            frames=[
                Frame("sin", "libm.so.6", 0, is_native=True),
                Frame("native_math_work", "examples/simple_profile.py", 42),
                Frame("mixed_workload", "examples/simple_profile.py", 49),
                Frame("main", "examples/simple_profile.py", 100),
            ],
        )
    )

    # Sample 4: Also in native math
    samples.append(
        Sample(
            timestamp_ns=1003000000,
            thread_id=1,
            thread_name="MainThread",
            frames=[
                Frame("cos", "libm.so.6", 0, is_native=True),
                Frame("native_math_work", "examples/simple_profile.py", 42),
                Frame("mixed_workload", "examples/simple_profile.py", 49),
                Frame("main", "examples/simple_profile.py", 100),
            ],
        )
    )

    # Add more fibonacci samples to show it's the hot spot
    for i in range(20):
        depth = 3 + (i % 5)
        frames = [Frame("pure_python_fibonacci", "examples/simple_profile.py", 25)] * depth
        frames.extend(
            [
                Frame("pure_python_work", "examples/simple_profile.py", 33),
                Frame("mixed_workload", "examples/simple_profile.py", 48),
                Frame("main", "examples/simple_profile.py", 100),
            ]
        )
        samples.append(
            Sample(
                timestamp_ns=1004000000 + i * 1000000,
                thread_id=1,
                thread_name="MainThread",
                frames=frames,
            )
        )

    # Add some native math samples
    for i in range(10):
        func = ["sin", "cos", "log", "sqrt", "exp"][i % 5]
        samples.append(
            Sample(
                timestamp_ns=1024000000 + i * 1000000,
                thread_id=1,
                thread_name="MainThread",
                frames=[
                    Frame(func, "libm.so.6", 0, is_native=True),
                    Frame("native_math_work", "examples/simple_profile.py", 42),
                    Frame("mixed_workload", "examples/simple_profile.py", 49),
                    Frame("main", "examples/simple_profile.py", 100),
                ],
            )
        )

    return Profile(
        start_time=datetime.now(),
        end_time=datetime.now(),
        interval_ms=1,
        samples=samples,
        dropped_count=0,
        python_version=f"{sys.version_info.major}.{sys.version_info.minor}.{sys.version_info.micro}",
        platform="Linux-demo",
    )


def main():
    print("=" * 60)
    print("Simple Profiling Demo")
    print("=" * 60)

    # Check native unwinding
    print(f"\nNative unwinding available: {spprof.native_unwinding_available()}")

    # Run workload (without profiling for this demo)
    results = mixed_workload()

    print("\n" + "=" * 60)
    print("Workload Results:")
    for key, value in results.items():
        print(f"  {key}: {value}")

    # Create demonstration profile
    print("\nCreating demonstration profile...")
    profile = create_demo_profile()

    # Print profile stats
    print("\n" + "=" * 60)
    print("Profile Statistics:")
    print(f"  Samples collected: {len(profile.samples)}")

    python_frames = sum(1 for s in profile.samples for f in s.frames if not f.is_native)
    native_frames = sum(1 for s in profile.samples for f in s.frames if f.is_native)
    print(f"  Python frames: {python_frames}")
    print(f"  Native frames: {native_frames}")

    # Save profiles
    output_dir = Path(__file__).parent.parent

    speedscope_path = output_dir / "demo_profile.json"
    profile.save(speedscope_path, format="speedscope")
    print(f"\nSpeedscope profile saved: {speedscope_path}")

    collapsed_path = output_dir / "demo_profile.collapsed"
    profile.save(collapsed_path, format="collapsed")
    print(f"Collapsed profile saved: {collapsed_path}")

    # Show sample of collapsed format
    print("\n" + "=" * 60)
    print("Collapsed format sample:")
    collapsed = profile.to_collapsed()
    for line in collapsed.split("\n")[:10]:
        print(f"  {line}")
    if len(collapsed.split("\n")) > 10:
        print(f"  ... and {len(collapsed.split(chr(10))) - 10} more lines")

    print("\n" + "=" * 60)
    print("To view the flame graph:")
    print("  1. Open https://www.speedscope.app")
    print(f"  2. Drag and drop: {speedscope_path}")
    print("=" * 60)


if __name__ == "__main__":
    main()
