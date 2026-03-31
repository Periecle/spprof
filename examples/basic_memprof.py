#!/usr/bin/env python3
"""Example: Basic Memory Profiling

This example demonstrates the simplest usage of the memory profiler.

Task: T114 - Create basic_profile.py example
"""


def main():
    print("Basic Memory Profiling Example")
    print("=" * 40)

    import spprof.memprof as memprof

    # Start profiling with default settings (512KB sampling rate)
    print("\n1. Starting memory profiler...")
    memprof.start()

    # Do some memory-intensive work
    print("2. Running workload...")

    # Create some data structures
    numbers = [i ** 2 for i in range(100000)]
    strings = [f"item_{i}" for i in range(10000)]
    nested = [[j for j in range(100)] for i in range(100)]

    # Get current state
    print("3. Capturing snapshot...")
    snapshot = memprof.get_snapshot()
    stats = memprof.get_stats()

    # Display results
    print("\n" + "=" * 40)
    print("Memory Profile Results")
    print("=" * 40)

    print(f"\nSampling rate: {stats.sampling_rate_bytes / 1024:.0f} KB")
    print(f"Total samples: {stats.total_samples}")
    print(f"Live samples: {stats.live_samples}")
    print(f"Estimated heap: {snapshot.estimated_heap_bytes / 1e6:.2f} MB")

    # Show top allocators
    print("\nTop allocation sites:")
    for i, site in enumerate(snapshot.top_allocators(5), 1):
        print(f"  {i}. {site['function']} ({site['file']}:{site['line']})")
        print(f"     {site['estimated_bytes'] / 1e6:.2f} MB ({site['sample_count']} samples)")

    # Stop profiling
    print("\n4. Stopping profiler...")
    memprof.stop()

    # Optional: save to file
    # snapshot.save("memory_profile.json")
    # print("5. Saved to memory_profile.json")

    print("\nDone!")

    # Clean up
    del numbers, strings, nested


if __name__ == "__main__":
    main()

