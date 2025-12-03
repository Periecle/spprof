#!/usr/bin/env python3
"""Memory Profiler Review Example

Generates memory profile data for manual review in Speedscope and FlameGraph.

Outputs:
- memprof_review.json  - Speedscope format (open at https://speedscope.app)
- memprof_review.collapsed - Collapsed format (for FlameGraph)
"""

import gc
import random
import time
from pathlib import Path


def allocate_strings(count: int, size: int) -> list:
    """Allocate many strings of given size."""
    return [f"string_{i}_" + "x" * size for i in range(count)]


def allocate_lists(count: int, size: int) -> list:
    """Allocate nested lists."""
    return [[j for j in range(size)] for i in range(count)]


def allocate_dicts(count: int) -> list:
    """Allocate dictionaries with random data."""
    return [
        {"id": i, "name": f"item_{i}", "values": list(range(100))}
        for i in range(count)
    ]


def allocate_bytearrays(count: int, size: int) -> list:
    """Allocate bytearrays."""
    return [bytearray(size) for _ in range(count)]


def recursive_allocator(depth: int, width: int) -> list:
    """Allocate in a recursive pattern to create deeper stacks."""
    if depth <= 0:
        return [bytearray(1024) for _ in range(width)]
    return [recursive_allocator(depth - 1, width) for _ in range(width)]


def simulate_data_processing():
    """Simulate a data processing workload."""
    # Load some "data"
    raw_data = allocate_strings(1000, 100)
    
    # Process it
    processed = []
    for item in raw_data:
        processed.append(item.upper())
    
    # Aggregate results
    results = allocate_dicts(500)
    
    return results


def simulate_cache_operations():
    """Simulate cache-like operations with churn."""
    cache = {}
    
    for i in range(2000):
        key = f"key_{i % 100}"
        if key in cache:
            # Update existing
            cache[key] = allocate_bytearrays(10, 256)
        else:
            # New entry
            cache[key] = allocate_bytearrays(20, 128)
        
        # Evict old entries periodically
        if i % 50 == 0:
            keys_to_remove = list(cache.keys())[:10]
            for k in keys_to_remove:
                del cache[k]
    
    return cache


def main():
    import spprof.memprof as memprof
    
    output_dir = Path(__file__).parent.parent
    speedscope_path = output_dir / "memprof_review.json"
    collapsed_path = output_dir / "memprof_review.collapsed"
    
    print("=" * 60)
    print("Memory Profiler Review Example")
    print("=" * 60)
    
    # Force GC before starting
    gc.collect()
    
    print("\n[1/5] Starting memory profiler (64KB sampling rate)...")
    memprof.start(sampling_rate_kb=64)  # Lower rate = more samples
    
    print("[2/5] Running workloads...")
    
    # Various allocation patterns
    print("  - Allocating strings...")
    strings = allocate_strings(5000, 50)
    
    print("  - Allocating lists...")
    lists = allocate_lists(500, 200)
    
    print("  - Allocating dicts...")
    dicts = allocate_dicts(1000)
    
    print("  - Allocating bytearrays...")
    bytearrays = allocate_bytearrays(1000, 4096)
    
    print("  - Recursive allocations...")
    recursive = recursive_allocator(4, 5)
    
    print("  - Simulating data processing...")
    processed = simulate_data_processing()
    
    print("  - Simulating cache operations...")
    cache = simulate_cache_operations()
    
    # Small delay to let things settle
    time.sleep(0.1)
    
    print("\n[3/5] Stopping profiler (resolves symbols)...")
    memprof.stop()
    
    print("[4/5] Capturing snapshot...")
    snapshot = memprof.get_snapshot()
    stats = memprof.get_stats()
    
    # Print statistics
    print("\n" + "=" * 60)
    print("MEMORY PROFILE STATISTICS")
    print("=" * 60)
    print(f"  Sampling rate:      {stats.sampling_rate_bytes / 1024:.0f} KB")
    print(f"  Total samples:      {stats.total_samples}")
    print(f"  Live samples:       {stats.live_samples}")
    print(f"  Freed samples:      {stats.freed_samples}")
    print(f"  Unique stacks:      {stats.unique_stacks}")
    print(f"  Estimated heap:     {snapshot.estimated_heap_bytes / 1e6:.2f} MB")
    print(f"  Heap map load:      {stats.heap_map_load_percent:.4f}%")
    print(f"  Collisions:         {stats.collisions}")
    
    # Frame pointer health
    fp = snapshot.frame_pointer_health
    print(f"\n  Frame Pointer Health:")
    print(f"    Total native stacks:   {fp.total_native_stacks}")
    print(f"    Avg native depth:      {fp.avg_native_depth:.1f}")
    print(f"    Truncation rate:       {fp.truncation_rate:.1%}")
    print(f"    Confidence:            {fp.confidence}")
    
    # Top allocators
    print("\n" + "-" * 60)
    print("TOP ALLOCATION SITES (by estimated bytes)")
    print("-" * 60)
    top = snapshot.top_allocators(10)
    for i, site in enumerate(top, 1):
        mb = site['estimated_bytes'] / 1e6
        print(f"  {i:2}. {site['function']}")
        print(f"      {site['file']}:{site['line']}")
        print(f"      {mb:.2f} MB ({site['sample_count']} samples)")
        print()
    
    print("[5/5] Saving output files...")
    
    # Save Speedscope format
    snapshot.save(speedscope_path, format="speedscope")
    print(f"  ✓ Speedscope: {speedscope_path}")
    print(f"    Open at: https://speedscope.app")
    
    # Save collapsed format
    snapshot.save(collapsed_path, format="collapsed")
    print(f"  ✓ Collapsed: {collapsed_path}")
    print(f"    Use with: flamegraph.pl {collapsed_path} > memprof.svg")
    
    print("\n" + "=" * 60)
    print("Done! Review the output files to analyze memory allocations.")
    print("=" * 60)
    
    # Clean up
    del strings, lists, dicts, bytearrays, recursive, processed, cache


if __name__ == "__main__":
    main()

