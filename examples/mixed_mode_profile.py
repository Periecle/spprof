#!/usr/bin/env python3
"""
Mixed-mode profiling example - captures both Python and native C frames.

This example does CPU-intensive work using both pure Python and C extensions
to demonstrate mixed-mode flame graphs.

Run on Linux/WSL to get native frames:
    python examples/mixed_mode_profile.py

Output:
    - mixed_profile.json (Speedscope format)
    - mixed_profile.collapsed (FlameGraph format)
"""

import hashlib
import math
import re
import sys
from pathlib import Path


# Add src to path for development
sys.path.insert(0, str(Path(__file__).parent.parent / "src"))

import spprof


def pure_python_fibonacci(n: int) -> int:
    """Pure Python recursive Fibonacci - creates deep Python call stacks."""
    if n <= 1:
        return n
    return pure_python_fibonacci(n - 1) + pure_python_fibonacci(n - 2)


def pure_python_prime_check(n: int) -> bool:
    """Pure Python prime checking."""
    if n < 2:
        return False
    return all(n % i != 0 for i in range(2, int(n**0.5) + 1))


def pure_python_work():
    """Pure Python CPU-intensive work."""
    print("  Running pure Python work...")

    # Fibonacci creates deep recursion
    results = []
    for i in range(28):
        results.append(pure_python_fibonacci(i))

    # Prime checking
    primes = [n for n in range(10000) if pure_python_prime_check(n)]

    return len(results), len(primes)


def native_math_work():
    """Work using C math library functions."""
    print("  Running native math operations...")

    result = 0.0
    for i in range(1, 100000):
        x = float(i)
        # These call into C library
        result += math.sin(x) * math.cos(x)
        result += math.log(x) * math.sqrt(x)
        result += math.exp(-x / 100000)

    return result


def native_hash_work():
    """Work using C hashlib (OpenSSL)."""
    print("  Running native hash operations...")

    data = b"x" * 1000
    results = []

    for i in range(10000):
        # SHA256 is implemented in C
        h = hashlib.sha256()
        h.update(data)
        h.update(str(i).encode())
        results.append(h.digest())

    return len(results)


def native_regex_work():
    """Work using C regex engine."""
    print("  Running native regex operations...")

    text = "The quick brown fox jumps over the lazy dog. " * 100
    patterns = [
        r"\b\w{4,}\b",
        r"[aeiou]+",
        r"\s+",
        r"[A-Z][a-z]*",
    ]

    total_matches = 0
    for _ in range(100):
        for pattern in patterns:
            matches = re.findall(pattern, text)
            total_matches += len(matches)

    return total_matches


def mixed_workload():
    """Run a mixed workload with both Python and native code."""
    print("\nRunning mixed workload...")

    results = {}

    # Pure Python (deep call stacks)
    fib_count, prime_count = pure_python_work()
    results["fibonacci"] = fib_count
    results["primes"] = prime_count

    # Native C library calls
    results["math"] = native_math_work()
    results["hashes"] = native_hash_work()
    results["regex_matches"] = native_regex_work()

    return results


def main():
    print("=" * 60)
    print("Mixed-Mode Profiling Example")
    print("=" * 60)

    # Check if native unwinding is available
    native_available = spprof.native_unwinding_available()
    print(f"\nNative unwinding available: {native_available}")

    if native_available:
        print("Enabling native C-stack unwinding...")
        spprof.set_native_unwinding(True)
        print(f"Native unwinding enabled: {spprof.native_unwinding_enabled()}")
    else:
        print("WARNING: Native unwinding not available.")
        print("         Run on Linux/macOS for mixed-mode profiling.")

    # Start profiling with fast sampling for better resolution
    print("\nStarting profiler (1ms sampling interval)...")
    spprof.start(interval_ms=1)

    # Run workload
    results = mixed_workload()

    # Stop profiling
    profile = spprof.stop()

    # Print results
    print("\n" + "=" * 60)
    print("Workload Results:")
    for key, value in results.items():
        print(f"  {key}: {value}")

    # Print profile stats
    print("\n" + "=" * 60)
    print("Profile Statistics:")
    print(f"  Samples collected: {len(profile.samples)}")
    print(f"  Dropped samples: {profile.dropped_count}")
    print(f"  Duration: {(profile.end_time - profile.start_time).total_seconds():.3f}s")
    print(f"  Python version: {profile.python_version}")
    print(f"  Platform: {profile.platform}")

    # Analyze frames
    python_frames = 0
    native_frames = 0
    unique_functions = set()

    for sample in profile.samples:
        for frame in sample.frames:
            if frame.is_native:
                native_frames += 1
            else:
                python_frames += 1
            unique_functions.add(frame.function_name)

    print(f"\n  Python frames: {python_frames}")
    print(f"  Native frames: {native_frames}")
    print(f"  Unique functions: {len(unique_functions)}")

    # Save profiles
    output_dir = Path(__file__).parent.parent

    speedscope_path = output_dir / "mixed_profile.json"
    profile.save(speedscope_path, format="speedscope")
    print(f"\nSpeedscope profile saved: {speedscope_path}")

    collapsed_path = output_dir / "mixed_profile.collapsed"
    profile.save(collapsed_path, format="collapsed")
    print(f"Collapsed profile saved: {collapsed_path}")

    # Print some sample stacks
    print("\n" + "=" * 60)
    print("Sample Stack Traces (first 3):")
    for i, sample in enumerate(profile.samples[:3]):
        print(f"\n  Sample {i + 1} (thread {sample.thread_id}):")
        for j, frame in enumerate(sample.frames[:10]):
            native_marker = " [NATIVE]" if frame.is_native else ""
            print(
                f"    {j}: {frame.function_name} ({frame.filename}:{frame.lineno}){native_marker}"
            )
        if len(sample.frames) > 10:
            print(f"    ... and {len(sample.frames) - 10} more frames")

    print("\n" + "=" * 60)
    print("To view the flame graph:")
    print("  1. Open https://www.speedscope.app")
    print(f"  2. Drag and drop: {speedscope_path}")
    print("=" * 60)


if __name__ == "__main__":
    main()
