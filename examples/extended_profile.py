#!/usr/bin/env python3
"""
Extended profiling example - runs for longer to collect more samples.

This example runs CPU-intensive workloads for ~10 seconds to collect
a meaningful number of samples for flame graph visualization.

Run:
    python examples/extended_profile.py

Output:
    - extended_profile.json (Speedscope format)
    - extended_profile.collapsed (FlameGraph format)
    - extended_profile.svg (Interactive flame graph)
"""

import hashlib
import json
import math
import random
import re
import struct
import sys
import zlib
from pathlib import Path


# Add src to path for development
sys.path.insert(0, str(Path(__file__).parent.parent / "src"))

import spprof


# =============================================================================
# Pure Python Workloads (visible in Python frames)
# =============================================================================


def fibonacci_iterative(n: int) -> int:
    """Iterative Fibonacci - less stack depth but CPU intensive."""
    if n <= 1:
        return n
    a, b = 0, 1
    for _ in range(2, n + 1):
        a, b = b, a + b
    return b


def fibonacci_recursive(n: int) -> int:
    """Recursive Fibonacci - creates deep Python call stacks."""
    if n <= 1:
        return n
    return fibonacci_recursive(n - 1) + fibonacci_recursive(n - 2)


def matrix_multiply(a: list[list[float]], b: list[list[float]]) -> list[list[float]]:
    """Pure Python matrix multiplication."""
    n = len(a)
    m = len(b[0])
    k = len(b)
    result = [[0.0] * m for _ in range(n)]
    for i in range(n):
        for j in range(m):
            for idx in range(k):
                result[i][j] += a[i][idx] * b[idx][j]
    return result


def create_random_matrix(rows: int, cols: int) -> list[list[float]]:
    """Create a random matrix."""
    return [[random.random() for _ in range(cols)] for _ in range(rows)]


def quicksort(arr: list[int]) -> list[int]:
    """Pure Python quicksort implementation."""
    if len(arr) <= 1:
        return arr
    pivot = arr[len(arr) // 2]
    left = [x for x in arr if x < pivot]
    middle = [x for x in arr if x == pivot]
    right = [x for x in arr if x > pivot]
    return quicksort(left) + middle + quicksort(right)


def merge_sort(arr: list[int]) -> list[int]:
    """Pure Python merge sort implementation."""
    if len(arr) <= 1:
        return arr
    mid = len(arr) // 2
    left = merge_sort(arr[:mid])
    right = merge_sort(arr[mid:])
    return merge(left, right)


def merge(left: list[int], right: list[int]) -> list[int]:
    """Merge two sorted lists."""
    result = []
    i = j = 0
    while i < len(left) and j < len(right):
        if left[i] <= right[j]:
            result.append(left[i])
            i += 1
        else:
            result.append(right[j])
            j += 1
    result.extend(left[i:])
    result.extend(right[j:])
    return result


def is_prime(n: int) -> bool:
    """Check if a number is prime."""
    if n < 2:
        return False
    if n == 2:
        return True
    if n % 2 == 0:
        return False
    return all(n % i != 0 for i in range(3, int(n**0.5) + 1, 2))


def find_primes_in_range(start: int, end: int) -> list[int]:
    """Find all primes in a range."""
    return [n for n in range(start, end) if is_prime(n)]


# =============================================================================
# Native Library Workloads (calls into C libraries)
# =============================================================================


def intensive_math_operations(iterations: int) -> float:
    """Intensive math operations using C math library."""
    result = 0.0
    for i in range(1, iterations + 1):
        x = float(i)
        # Trigonometric (libm)
        result += math.sin(x) * math.cos(x)
        result += math.tan(x * 0.0001)
        # Exponential/logarithmic
        result += math.log(x) * math.sqrt(x)
        result += math.exp(-x / iterations)
        # Powers
        result += math.pow(x, 0.5)
        # Hyperbolic
        result += math.sinh(x * 0.0001) + math.cosh(x * 0.0001)
    return result


def intensive_hashing(data: bytes, iterations: int) -> list[bytes]:
    """Intensive hashing operations using OpenSSL/CommonCrypto."""
    results = []
    for i in range(iterations):
        # SHA-256 (C implementation)
        h256 = hashlib.sha256()
        h256.update(data)
        h256.update(str(i).encode())
        digest256 = h256.digest()

        # SHA-512 (C implementation)
        h512 = hashlib.sha512()
        h512.update(data)
        h512.update(digest256)

        # MD5 (C implementation)
        hmd5 = hashlib.md5()
        hmd5.update(h512.digest())

        results.append(hmd5.digest())
    return results


def intensive_regex(text: str, iterations: int) -> int:
    """Intensive regex operations using C regex engine."""
    patterns = [
        r"\b\w{4,}\b",  # Words with 4+ chars
        r"[aeiou]+",  # Vowel sequences
        r"\s+",  # Whitespace
        r"[A-Z][a-z]*",  # Capitalized words
        r"\d+",  # Numbers
        r"[.,!?;:]",  # Punctuation
        r"\b(the|and|of)\b",  # Common words
        r"[bcdfghjklmnpqrstvwxyz]+",  # Consonant sequences
    ]

    total_matches = 0
    for _ in range(iterations):
        for pattern in patterns:
            matches = re.findall(pattern, text, re.IGNORECASE)
            total_matches += len(matches)
    return total_matches


def intensive_compression(data: bytes, iterations: int) -> tuple[int, int]:
    """Intensive compression/decompression using zlib (C library)."""
    total_compressed = 0
    total_decompressed = 0

    for i in range(iterations):
        # Modify data slightly each iteration
        modified = data + str(i).encode()

        # Compress
        compressed = zlib.compress(modified, level=6)
        total_compressed += len(compressed)

        # Decompress
        decompressed = zlib.decompress(compressed)
        total_decompressed += len(decompressed)

    return total_compressed, total_decompressed


def intensive_json_operations(iterations: int) -> int:
    """Intensive JSON encoding/decoding (C extension in Python 3.x)."""
    total_size = 0

    for i in range(iterations):
        # Create complex nested structure
        data = {
            "id": i,
            "name": f"item_{i}",
            "values": [x * 0.1 for x in range(100)],
            "nested": {"level1": {"level2": {"level3": {"data": list(range(50))}}}},
            "tags": [f"tag_{j}" for j in range(20)],
        }

        # Encode to JSON (C extension)
        encoded = json.dumps(data)
        total_size += len(encoded)

        # Decode from JSON (C extension)
        decoded = json.loads(encoded)
        total_size += len(str(decoded))

    return total_size


def intensive_struct_packing(iterations: int) -> int:
    """Intensive struct packing/unpacking (C module)."""
    total_bytes = 0

    for i in range(iterations):
        # Pack various types
        packed = struct.pack(
            "<iIhHbBfddQ",
            i,
            i * 2,
            i % 32768,
            i % 65536,
            i % 128,
            i % 256,
            float(i),
            float(i) * 1.5,
            float(i) * 2.0,
            i * 100,
        )
        total_bytes += len(packed)

        # Unpack
        unpacked = struct.unpack("<iIhHbBfddQ", packed)
        total_bytes += len(unpacked)

    return total_bytes


# =============================================================================
# Workload Runner
# =============================================================================


def run_python_workloads(duration_hint: int = 3):
    """Run pure Python workloads."""
    print("  [Python] Running recursive Fibonacci...")
    for i in range(30):
        fibonacci_recursive(i)

    print("  [Python] Running iterative Fibonacci...")
    for _ in range(1000):
        for i in range(100):
            fibonacci_iterative(i)

    print("  [Python] Running matrix operations...")
    for _ in range(50):
        a = create_random_matrix(30, 30)
        b = create_random_matrix(30, 30)
        matrix_multiply(a, b)

    print("  [Python] Running sorting algorithms...")
    for _ in range(100):
        arr = [random.randint(0, 10000) for _ in range(500)]
        quicksort(arr.copy())
        merge_sort(arr.copy())

    print("  [Python] Finding primes...")
    for _ in range(10):
        find_primes_in_range(2, 10000)


def run_native_workloads(duration_hint: int = 7):
    """Run workloads that call into native C libraries."""
    print("  [Native] Running math operations...")
    intensive_math_operations(500000)

    print("  [Native] Running hash operations...")
    data = b"x" * 2000
    intensive_hashing(data, 10000)

    print("  [Native] Running regex operations...")
    text = (
        """
    The quick brown fox jumps over the lazy dog.
    Pack my box with five dozen liquor jugs.
    How vexingly quick daft zebras jump!
    The five boxing wizards jump quickly.
    Jackdaws love my big sphinx of quartz.
    """
        * 50
    )
    intensive_regex(text, 200)

    print("  [Native] Running compression operations...")
    data = b"Sample data for compression. " * 500
    intensive_compression(data, 500)

    print("  [Native] Running JSON operations...")
    intensive_json_operations(2000)

    print("  [Native] Running struct operations...")
    intensive_struct_packing(50000)


# =============================================================================
# Main
# =============================================================================


def main():
    print("=" * 70)
    print("Extended Profiling Example")
    print("=" * 70)

    # Check capabilities
    native_available = spprof.native_unwinding_available()
    print(f"\nNative unwinding available: {native_available}")

    if native_available:
        print("Enabling native C-stack unwinding...")
        spprof.set_native_unwinding(True)
        print(f"Native unwinding enabled: {spprof.native_unwinding_enabled()}")

    # Start profiling with 1ms sampling
    print("\nStarting profiler (1ms sampling interval)...")
    print("Running workloads for ~10 seconds...")
    spprof.start(interval_ms=1)

    # Run workloads
    print("\n" + "-" * 70)
    print("Phase 1: Python-heavy workloads")
    print("-" * 70)
    run_python_workloads()

    print("\n" + "-" * 70)
    print("Phase 2: Native library workloads")
    print("-" * 70)
    run_native_workloads()

    # Stop profiling
    profile = spprof.stop()

    # Print profile stats
    print("\n" + "=" * 70)
    print("Profile Statistics:")
    print("=" * 70)
    print(f"  Samples collected: {len(profile.samples)}")
    print(f"  Dropped samples: {profile.dropped_count}")
    duration = (profile.end_time - profile.start_time).total_seconds()
    print(f"  Duration: {duration:.3f}s")
    print(f"  Sample rate: {len(profile.samples) / duration:.1f} samples/sec")
    print(f"  Python version: {profile.python_version}")
    print(f"  Platform: {profile.platform}")

    # Analyze frames
    python_frames = 0
    native_frames = 0
    unique_functions = set()
    function_counts = {}

    for sample in profile.samples:
        for frame in sample.frames:
            if frame.is_native:
                native_frames += 1
            else:
                python_frames += 1
            func_name = frame.function_name
            unique_functions.add(func_name)
            function_counts[func_name] = function_counts.get(func_name, 0) + 1

    print(f"\n  Python frames: {python_frames}")
    print(f"  Native frames: {native_frames}")
    print(f"  Unique functions: {len(unique_functions)}")

    # Top functions by sample count
    print("\n  Top 15 functions by sample count:")
    sorted_funcs = sorted(function_counts.items(), key=lambda x: x[1], reverse=True)[:15]
    for func, count in sorted_funcs:
        pct = 100.0 * count / sum(function_counts.values())
        print(f"    {count:5d} ({pct:5.1f}%): {func}")

    # Save profiles
    output_dir = Path(__file__).parent.parent

    speedscope_path = output_dir / "extended_profile.json"
    profile.save(speedscope_path, format="speedscope")
    print(f"\nSpeedscope profile saved: {speedscope_path}")

    collapsed_path = output_dir / "extended_profile.collapsed"
    profile.save(collapsed_path, format="collapsed")
    print(f"Collapsed profile saved: {collapsed_path}")

    # Generate SVG flamegraph if flamegraph.pl is available
    import shutil
    import subprocess

    svg_path = output_dir / "extended_profile.svg"

    # Try common locations for flamegraph.pl
    flamegraph_locations = [
        "/tmp/flamegraph.pl",
        "/usr/local/bin/flamegraph.pl",
        shutil.which("flamegraph.pl"),
    ]

    flamegraph_script = None
    for loc in flamegraph_locations:
        if loc and Path(loc).exists():
            flamegraph_script = loc
            break

    if flamegraph_script:
        print(f"\nGenerating SVG flamegraph using {flamegraph_script}...")
        try:
            with collapsed_path.open() as f_in:
                result = subprocess.run(
                    ["perl", flamegraph_script, "--title", "spprof Extended Profile"],
                    stdin=f_in,
                    capture_output=True,
                    text=True,
                )
                if result.returncode == 0:
                    with svg_path.open("w") as f_out:
                        f_out.write(result.stdout)
                    print(f"SVG flamegraph saved: {svg_path}")
                else:
                    print(f"Flamegraph generation failed: {result.stderr}")
        except Exception as e:
            print(f"Failed to generate SVG: {e}")
    else:
        print("\nNote: flamegraph.pl not found. Install from:")
        print("  https://github.com/brendangregg/FlameGraph")

    # Print sample stacks
    print("\n" + "=" * 70)
    print("Sample Stack Traces (5 random samples):")
    print("=" * 70)

    import random

    sample_indices = random.sample(range(len(profile.samples)), min(5, len(profile.samples)))

    for idx in sample_indices:
        sample = profile.samples[idx]
        print(f"\n  Sample {idx + 1} (thread {sample.thread_id}):")
        for j, frame in enumerate(sample.frames[:12]):
            native_marker = " [NATIVE]" if frame.is_native else ""
            # Shorten filename for display
            filename = frame.filename
            if len(filename) > 50:
                filename = "..." + filename[-47:]
            print(f"    {j:2d}: {frame.function_name} ({filename}:{frame.lineno}){native_marker}")
        if len(sample.frames) > 12:
            print(f"    ... and {len(sample.frames) - 12} more frames")

    print("\n" + "=" * 70)
    print("To view the flame graph interactively:")
    print("  1. Open https://www.speedscope.app")
    print(f"  2. Drag and drop: {speedscope_path}")
    print("=" * 70)


if __name__ == "__main__":
    main()
