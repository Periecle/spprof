# Profile a CPU-Intensive Function

Write a Python script that profiles a CPU-intensive function using a sampling profiler. The profiler should be started before the workload runs, then stopped to retrieve the profile results.

## Requirements

- Start the profiler with a 5ms sampling interval
- Run a workload that performs at least 1,000,000 iterations of arithmetic operations
- Stop the profiler and retrieve the profile object
- Print the number of samples collected
- Print the total profiling duration in milliseconds (rounded to 2 decimal places)

The profiler must be explicitly started and stopped using the procedural API (not a context manager or decorator).

## Test Cases

- Starting the profiler with `interval_ms=5` and running a loop of 1,000,000 iterations should produce a profile with `sample_count >= 0` @test
- After calling stop, `sample_count` should be an integer attribute of the returned profile object @test
- `total_duration_ms` should be a float representing elapsed time and should be greater than 0 @test
- Attempting to start the profiler a second time before stopping should raise a `RuntimeError` @test

## Dependencies { .dependencies }

### spprof 0.1.0 { .dependency }

High-performance sampling profiler for Python applications. Provides procedural `start()` and `stop()` functions for CPU profiling with configurable sampling intervals.
