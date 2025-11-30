#!/usr/bin/env python3
"""
Multi-threaded profiling example.

Usage:
    python examples/threaded_profile.py

This demonstrates profiling applications with multiple threads.
Each thread will appear as a separate profile in Speedscope.
"""

import threading

import spprof


def compute_primes(limit: int) -> list[int]:
    """Compute prime numbers up to limit (inefficient sieve)."""
    primes = []
    for num in range(2, limit):
        is_prime = True
        for i in range(2, int(num**0.5) + 1):
            if num % i == 0:
                is_prime = False
                break
        if is_prime:
            primes.append(num)
    return primes


def worker(worker_id: int, limit: int, results: dict):
    """Worker thread that computes primes."""
    thread_name = threading.current_thread().name
    print(f"  [{thread_name}] Starting computation (limit={limit})")

    primes = compute_primes(limit)

    results[worker_id] = len(primes)
    print(f"  [{thread_name}] Found {len(primes)} primes")


def main():
    print("Multi-threaded profiling example")
    print("=" * 40)

    num_threads = 4
    work_limit = 10000
    results: dict[int, int] = {}

    print(f"\nStarting {num_threads} worker threads...")
    print("Profiling with 5ms sampling interval...")

    # Start profiling
    spprof.start(interval_ms=5)

    # Create and start threads
    threads = []
    for i in range(num_threads):
        t = threading.Thread(
            target=worker, args=(i, work_limit + i * 1000, results), name=f"Worker-{i}"
        )
        threads.append(t)
        t.start()

    # Wait for all threads
    for t in threads:
        t.join()

    # Stop profiling
    profile = spprof.stop()

    print("\nResults:")
    for worker_id, prime_count in sorted(results.items()):
        print(f"  Worker-{worker_id}: {prime_count} primes")

    # Get unique thread IDs from samples
    thread_ids = {s.thread_id for s in profile.samples}
    thread_names = {s.thread_name for s in profile.samples if s.thread_name}

    print("\nProfile statistics:")
    print(f"  Total samples: {len(profile.samples)}")
    print(f"  Unique threads sampled: {len(thread_ids)}")
    print(f"  Thread names: {thread_names or 'N/A (no samples)'}")
    print(f"  Dropped samples: {profile.dropped_count}")

    # Save profile
    output_path = "threaded_profile.json"
    profile.save(output_path)
    print(f"\nProfile saved to: {output_path}")
    print("Open in Speedscope to see per-thread flame graphs")


if __name__ == "__main__":
    main()
