# Profile Multiple Worker Threads Using the Thread Context Manager

Use a sampling profiler library to profile multiple threads, ensuring each worker thread is correctly registered for profiling using the thread-local profiler context manager.

## Requirements

1. Start the global profiler with a 5ms sampling interval.
2. Launch 3 worker threads. In each worker thread, use the thread context manager (from the profiler library) to automatically register and unregister the thread for profiling around the workload. The workload is a tight loop computing `sum(range(100000))`.
3. Wait for all 3 threads to complete.
4. Stop the global profiler and capture the result.
5. Print the total number of samples collected across all threads.
6. Verify that the Profile contains samples from at least the main thread (thread_id is present in samples).

## Test Cases

- Start profiling, launch 3 worker threads using the thread context manager, join all threads, stop profiling, and verify sample_count >= 0. [@test](./test_thread_profiler_basic.py)
- Verify that the Profile's samples list contains Sample objects with valid thread_id (non-zero integer). [@test](./test_sample_thread_ids.py)
- Verify the thread context manager can be used in a thread that runs while the profiler is active. [@test](./test_thread_profiler_context.py)

## Dependencies { .dependencies }

### spprof 0.1.0 { .dependency }

High-performance sampling profiler for Python with thread-local profiling support.
