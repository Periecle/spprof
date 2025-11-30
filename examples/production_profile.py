#!/usr/bin/env python3
"""
Production profiling example - demonstrates real signal-based sampling.

This example uses the production-ready internal API mode for async-signal-safe
sampling. Unlike the demo examples, this captures real samples from the
running program.

Requirements:
    - Python 3.11+ (for internal API access)
    - Built with SPPROF_USE_INTERNAL_API=1 (default)
    - Linux or macOS (Windows uses different mechanism)

Usage:
    python examples/production_profile.py

Output:
    - production_profile.json (Speedscope format)
    - production_profile.collapsed (FlameGraph format)
"""

import hashlib
import math
import sys
import time
from pathlib import Path


# Add src to path for development
sys.path.insert(0, str(Path(__file__).parent.parent / "src"))

import spprof


def cpu_intensive_work():
    """Pure Python CPU-intensive work."""
    result = 0
    for i in range(1, 100000):
        result += math.sin(i) * math.cos(i)
    return result


def recursive_fibonacci(n: int) -> int:
    """Classic recursive Fibonacci - creates deep call stacks."""
    if n <= 1:
        return n
    return recursive_fibonacci(n - 1) + recursive_fibonacci(n - 2)


def hash_computation():
    """Computation that calls into C extensions (hashlib)."""
    data = b"x" * 10000
    result = data
    for _ in range(1000):
        result = hashlib.sha256(result).digest()
    return result


def mixed_workload():
    """Run a variety of workloads to demonstrate profiling."""
    results = {}

    print("  [1/4] Pure Python math...")
    results["math"] = cpu_intensive_work()

    print("  [2/4] Recursive computation...")
    results["fibonacci"] = recursive_fibonacci(30)

    print("  [3/4] Hash computation (C extension)...")
    results["hash"] = hash_computation()

    print("  [4/4] More math...")
    results["math2"] = cpu_intensive_work()

    return results


def check_environment():
    """Check if the environment supports production profiling."""
    print("=" * 70)
    print("Environment Check")
    print("=" * 70)

    # Python version
    py_version = f"{sys.version_info.major}.{sys.version_info.minor}.{sys.version_info.micro}"
    print(f"Python version: {py_version}")

    if sys.version_info < (3, 11):
        print("  ⚠️  WARNING: Python 3.11+ recommended for internal API access")
        print("      Current version may use fallback mode (not signal-safe)")
    else:
        print("  ✓  Python 3.11+ detected - internal API available")

    # Check native extension
    try:
        import spprof._native as native

        print("\nNative extension: loaded")

        if hasattr(native, "__version__"):
            print(f"  Version: {native.__version__}")
        if hasattr(native, "platform"):
            print(f"  Platform: {native.platform}")
        if hasattr(native, "frame_walker"):
            print(f"  Frame walker: {native.frame_walker}")
        if hasattr(native, "unwind_method"):
            print(f"  Unwind method: {native.unwind_method}")

        # Check for internal API mode
        frame_info = getattr(native, "frame_walker", "")
        if "internal" in frame_info.lower():
            print("  ✓  Internal API mode enabled (production-ready)")
        elif "public" in frame_info.lower():
            print("  ⚠️  Public API mode - may crash with signal sampling")

    except ImportError as e:
        print(f"\nNative extension: NOT loaded ({e})")
        print("  Build with: pip install -e .")
        return False

    # Check native unwinding
    print(f"\nNative unwinding available: {spprof.native_unwinding_available()}")

    print("=" * 70)
    return True


def main():
    print("\n" + "=" * 70)
    print("Production Profiling Demo")
    print("Real signal-based sampling with async-signal-safe frame walking")
    print("=" * 70 + "\n")

    if not check_environment():
        print("\n❌ Environment check failed. Please build the extension first.")
        return

    print("\n" + "=" * 70)
    print("Starting Profiler")
    print("=" * 70)

    # Enable native unwinding if available
    if spprof.native_unwinding_available():
        print("Enabling native C-stack unwinding...")
        spprof.set_native_unwinding(True)

    # Start profiling with 1ms interval
    interval_ms = 1
    print(f"Starting profiler with {interval_ms}ms sampling interval...")

    try:
        spprof.start(interval_ms=interval_ms)
        print("✓ Profiler started\n")

        # Run workload
        print("Running mixed workload...")
        start_time = time.perf_counter()
        _results = mixed_workload()
        elapsed = time.perf_counter() - start_time
        print(f"\nWorkload completed in {elapsed:.2f}s")

        # Get stats while running
        stats = spprof.stats()
        if stats:
            print("\nRunning statistics:")
            print(f"  Samples collected: {stats.collected_samples}")
            print(f"  Samples dropped: {stats.dropped_samples}")
            print(f"  Duration: {stats.duration_ms:.1f}ms")

        # Stop and get profile
        print("\nStopping profiler...")
        profile = spprof.stop()
        print("✓ Profiler stopped\n")

    except Exception as e:
        print(f"\n❌ Error during profiling: {e}")
        print("\nThis might happen if:")
        print("  - The C extension wasn't built with internal API")
        print("  - Platform doesn't support signal-based sampling")
        print("  - Running in an incompatible environment")

        if spprof.is_active():
            spprof.stop()
        return

    # Print profile statistics
    print("=" * 70)
    print("Profile Statistics")
    print("=" * 70)
    print(f"Samples collected: {len(profile.samples)}")
    print(f"Samples dropped: {profile.dropped_count}")
    print(f"Profiling interval: {profile.interval_ms}ms")
    print(f"Python version: {profile.python_version}")
    print(f"Platform: {profile.platform}")

    # Count frame types
    python_frames = sum(1 for s in profile.samples for f in s.frames if not f.is_native)
    native_frames = sum(1 for s in profile.samples for f in s.frames if f.is_native)
    print("\nFrame counts:")
    print(f"  Python frames: {python_frames}")
    print(f"  Native frames: {native_frames}")

    # Show unique functions
    functions = set()
    for sample in profile.samples:
        for frame in sample.frames:
            functions.add((frame.function_name, frame.filename))

    print(f"\nUnique functions captured: {len(functions)}")
    for func, filename in sorted(functions)[:20]:
        print(f"  {func} ({filename})")
    if len(functions) > 20:
        print(f"  ... and {len(functions) - 20} more")

    # Save profiles
    output_dir = Path(__file__).parent.parent

    speedscope_path = output_dir / "production_profile.json"
    profile.save(speedscope_path, format="speedscope")
    print(f"\n✓ Speedscope profile saved: {speedscope_path}")

    collapsed_path = output_dir / "production_profile.collapsed"
    profile.save(collapsed_path, format="collapsed")
    print(f"✓ Collapsed profile saved: {collapsed_path}")

    # Show sample of collapsed format
    print("\n" + "=" * 70)
    print("Collapsed Format Sample")
    print("=" * 70)
    collapsed = profile.to_collapsed()
    lines = collapsed.strip().split("\n")
    for line in lines[:10]:
        # Truncate long lines
        if len(line) > 100:
            line = line[:97] + "..."
        print(f"  {line}")
    if len(lines) > 10:
        print(f"  ... and {len(lines) - 10} more stacks")

    # Instructions for viewing
    print("\n" + "=" * 70)
    print("View Your Profile")
    print("=" * 70)
    print("1. Open https://www.speedscope.app")
    print(f"2. Drag and drop: {speedscope_path.absolute()}")
    print("\nOr use FlameGraph tools with the collapsed format:")
    print(f"  cat {collapsed_path} | flamegraph.pl > flame.svg")
    print("=" * 70 + "\n")


if __name__ == "__main__":
    main()
