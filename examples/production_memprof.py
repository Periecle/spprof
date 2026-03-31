#!/usr/bin/env python3
"""Example: Production Memory Profiling

This example demonstrates production-safe memory profiling practices:
- Using the context manager for automatic cleanup
- Handling low sample counts
- Monitoring profiler health
- Periodic snapshots for long-running processes

Task: T115 - Create production_profile.py example
"""

import gc
import time
from pathlib import Path


def simulate_production_workload():
    """Simulate a production workload with varying allocation patterns."""
    # Simulate data processing
    data = []
    for batch in range(10):
        # Process batch
        batch_data = [bytearray(1024) for _ in range(1000)]
        data.extend(batch_data[:100])  # Keep some, discard most
        time.sleep(0.01)  # Simulate I/O

    return data


def main():
    print("Production Memory Profiling Example")
    print("=" * 50)

    import spprof.memprof as memprof

    # Use context manager for automatic cleanup (recommended for production)
    print("\n1. Using context manager pattern...")

    with memprof.MemoryProfiler(sampling_rate_kb=512) as mp:
        # Run workload
        print("2. Running production workload...")
        retained_data = simulate_production_workload()
        print(f"   Retained {len(retained_data)} items")

    # After context exit, snapshot is available
    snapshot = mp.snapshot

    print("\n" + "=" * 50)
    print("Profile Results")
    print("=" * 50)

    print(f"\nLive samples: {snapshot.live_samples}")
    print(f"Estimated heap: {snapshot.estimated_heap_bytes / 1e6:.2f} MB")

    # Check data quality
    if snapshot.live_samples < 100:
        print(f"\n⚠️  Low sample count ({snapshot.live_samples})")
        print("   For more accurate results, use a lower sampling rate")
        print("   or profile a longer-running workload.")

    # Check frame pointer health
    health = snapshot.frame_pointer_health
    print(f"\nStack capture confidence: {health.confidence}")
    if health.recommendation:
        print(f"Recommendation: {health.recommendation}")

    # Save profile
    output_path = Path("production_memprofile.json")
    snapshot.save(output_path)
    print(f"\nSaved profile to {output_path}")

    # =========================================================================
    # Periodic monitoring pattern for long-running services
    # =========================================================================
    print("\n" + "=" * 50)
    print("Periodic Monitoring Example")
    print("=" * 50)

    memprof.start(sampling_rate_kb=1024)  # Higher rate = less overhead

    print("\nMonitoring for 3 iterations...")

    for i in range(3):
        # Simulate work
        work_data = simulate_production_workload()

        # Take periodic snapshot
        snap = memprof.get_snapshot()
        stats = memprof.get_stats()

        print(f"\n  Iteration {i + 1}:")
        print(f"    Live samples: {stats.live_samples}")
        print(f"    Estimated heap: {snap.estimated_heap_bytes / 1e6:.2f} MB")
        print(f"    Heap map load: {stats.heap_map_load_percent:.2f}%")

        # Check for potential issues
        if stats.heap_map_load_percent > 75:
            print("    ⚠️  High heap map load - consider shorter profiling windows")

        del work_data
        gc.collect()

    memprof.stop()

    # =========================================================================
    # Graceful shutdown
    # =========================================================================
    print("\n" + "=" * 50)
    print("Shutting down profiler...")

    # Shutdown releases resources (optional, automatic at process exit)
    memprof.shutdown()

    print("Done!")

    # Clean up
    del retained_data


if __name__ == "__main__":
    main()

