# Check Profiler Status and Live Statistics

Write a Python script that demonstrates how to check whether the profiler is currently running and how to retrieve live profiling statistics during an active session.

## Requirements

- Before starting, verify that the profiler is not active and print `"active before start: False"`
- Start the profiler with a 10ms interval
- After starting, verify the profiler is active and print `"active after start: True"`
- Call the function that retrieves live statistics and print the number of collected samples so far
- Run a workload (at least 500,000 iterations)
- Stop the profiler
- After stopping, verify the profiler is no longer active and print `"active after stop: False"`
- Verify that attempting to get statistics after stopping returns `None` and print `"stats after stop: None"`

## Test Cases

- Before start, the profiler status check returns `False` @test
- After start, the profiler status check returns `True` @test
- After stop, the profiler status check returns `False` @test
- The live statistics function returns `None` when profiling is not active @test

## Dependencies { .dependencies }

### spprof 0.1.0 { .dependency }

High-performance sampling profiler for Python applications. Provides `is_active()` to check profiler state and `stats()` to retrieve a `ProfilerStats` object (or `None` if inactive) with `collected_samples`, `dropped_samples`, `duration_ms`, and `overhead_estimate_pct`.
