#!/usr/bin/env python3
"""Example: Combined CPU and Memory Profiling

This example demonstrates running both CPU and memory profilers
simultaneously to get a complete picture of application performance.

Task: T094 - Document combined profiling
"""

import time


def compute_intensive():
    """CPU-bound computation."""
    result = 0
    for i in range(500000):
        result += i ** 2 + i ** 0.5
    return result


def memory_intensive():
    """Memory-bound work with allocations."""
    # Large list allocation
    data = [i ** 2 for i in range(100000)]
    
    # Dictionary with string keys
    lookup = {f"key_{i}": i * 2 for i in range(10000)}
    
    # Nested structure
    nested = [[j for j in range(100)] for i in range(1000)]
    
    return len(data) + len(lookup) + len(nested)


def mixed_workload():
    """Workload with both CPU and memory pressure."""
    # Memory allocation
    buffer = bytearray(1024 * 1024)  # 1MB
    
    # CPU computation using the buffer
    for i in range(len(buffer)):
        buffer[i] = (i * 7 + 13) % 256
    
    # More allocations
    chunks = [bytearray(4096) for _ in range(100)]
    
    return sum(len(c) for c in chunks)


def main():
    print("Combined CPU + Memory Profiling Example")
    print("=" * 50)
    
    try:
        import spprof
        import spprof.memprof as memprof
    except ImportError:
        print("Error: spprof not installed. Run: pip install spprof")
        return
    
    # Start both profilers
    print("\n1. Starting profilers...")
    spprof.start(interval_ms=5)  # CPU profiler at 5ms intervals
    memprof.start(sampling_rate_kb=128)  # Memory at 128KB sampling
    
    print("2. Running mixed workload...")
    
    # Run workloads
    t1 = time.perf_counter()
    
    cpu_result = compute_intensive()
    mem_result = memory_intensive()
    mix_result = mixed_workload()
    
    elapsed = time.perf_counter() - t1
    
    print(f"   Workload completed in {elapsed:.2f}s")
    print(f"   Results: CPU={cpu_result:.0f}, Mem={mem_result}, Mix={mix_result}")
    
    # Get memory snapshot before stopping
    print("\n3. Capturing profiles...")
    mem_snapshot = memprof.get_snapshot()
    mem_stats = memprof.get_stats()
    
    # Stop profilers
    cpu_profile = spprof.stop()
    memprof.stop()
    
    # Display CPU profile summary
    print("\n" + "=" * 50)
    print("CPU Profile Summary")
    print("=" * 50)
    print(f"  Interval: {cpu_profile.interval_ms}ms")
    print(f"  Samples: {len(cpu_profile.samples)}")
    print(f"  Duration: {cpu_profile.duration_ms:.1f}ms")
    
    # Show top CPU functions
    if hasattr(cpu_profile, 'top_functions'):
        print("\n  Top functions by CPU time:")
        for func in cpu_profile.top_functions(5):
            print(f"    {func['function']}: {func['self_percent']:.1f}%")
    
    # Display memory profile summary
    print("\n" + "=" * 50)
    print("Memory Profile Summary")
    print("=" * 50)
    print(f"  Sampling rate: {mem_stats.sampling_rate_bytes / 1024:.0f} KB")
    print(f"  Total samples: {mem_stats.total_samples}")
    print(f"  Live samples: {mem_stats.live_samples}")
    print(f"  Freed samples: {mem_stats.freed_samples}")
    print(f"  Unique stacks: {mem_stats.unique_stacks}")
    print(f"  Estimated heap: {mem_stats.estimated_heap_bytes / 1e6:.2f} MB")
    print(f"  Heap map load: {mem_stats.heap_map_load_percent:.2f}%")
    
    # Show top memory allocators
    print("\n  Top allocators by memory:")
    for site in mem_snapshot.top_allocators(5):
        print(f"    {site['function']} ({site['file']}:{site['line']})")
        print(f"      {site['estimated_bytes'] / 1e6:.2f} MB across {site['sample_count']} samples")
    
    # Frame pointer health
    health = mem_snapshot.frame_pointer_health
    print(f"\n  Stack capture confidence: {health.confidence}")
    if health.recommendation:
        print(f"  Recommendation: {health.recommendation}")
    
    # Save profiles
    print("\n4. Saving profiles...")
    cpu_profile.save("combined_cpu.json")
    mem_snapshot.save("combined_memory.json", format="speedscope")
    print("   Saved: combined_cpu.json, combined_memory.json")
    print("   View at https://speedscope.app")
    
    print("\nDone!")


if __name__ == "__main__":
    main()

