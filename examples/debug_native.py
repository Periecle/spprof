#!/usr/bin/env python3
"""
Debug script to check native frame capture.
"""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent.parent / "src"))

import spprof

print("=" * 60)
print("Native Frame Debug")
print("=" * 60)

# Check capabilities
print(f"\nNative unwinding available: {spprof.native_unwinding_available()}")
spprof.set_native_unwinding(True)
print(f"Native unwinding enabled: {spprof.native_unwinding_enabled()}")

# Simple profiling test
print("\n" + "-" * 60)
print("Running simple profile...")
print("-" * 60)

import math

def compute_heavy():
    """Heavy computation with native math calls."""
    result = 0.0
    for i in range(1, 100000):
        result += math.sin(float(i)) * math.cos(float(i))
        result += math.sqrt(float(i))
    return result

spprof.start(interval_ms=1)

# Do computation
result = compute_heavy()
print(f"Computation result: {result}")

profile = spprof.stop()

print(f"\nSamples collected: {len(profile.samples)}")

# Count frame types
python_frames = 0
native_frames = 0

for sample in profile.samples:
    for frame in sample.frames:
        if frame.is_native:
            native_frames += 1
        else:
            python_frames += 1

print(f"Python frames: {python_frames}")
print(f"Native frames: {native_frames}")

# Show first few samples in detail
print("\n" + "-" * 60)
print("First 3 sample details:")
print("-" * 60)

for i, sample in enumerate(profile.samples[:3]):
    print(f"\nSample {i + 1}:")
    for j, frame in enumerate(sample.frames):
        tag = "[NATIVE]" if frame.is_native else "[PYTHON]"
        print(f"  {j:2d} {tag} {frame.function_name}")
        print(f"       file: {frame.filename}")
        print(f"       line: {frame.lineno}")

