# Profile Code Using a Context Manager

Write a Python script that profiles a CPU-intensive computation using the context manager API of a sampling profiler. The context manager should automatically start and stop profiling around the code block.

## Requirements

- Use the context manager form (the `with` statement) to profile a workload
- Configure the profiler with a 10ms sampling interval
- After the context block exits, access the profile result from the context manager object
- Save the profile to a file named `output.json`
- Print the effective sampling rate in Hz (samples per second)

## Test Cases

- The context manager must start profiling on entry and stop on exit @test
- After the `with` block, the profile attribute on the context manager object must not be `None` @test
- `profile.save("output.json")` must create a file at the given path @test
- `effective_rate_hz` should be a non-negative float @test

## Dependencies { .dependencies }

### spprof 0.1.0 { .dependency }

High-performance sampling profiler for Python applications. Provides a `Profiler` context manager class for profiling code blocks, with access to the `Profile` result via the context manager instance.
