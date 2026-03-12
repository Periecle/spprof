# Profile a Function Using Start/Stop API

Use a sampling profiler library to measure the CPU usage of a simple workload using the functional start/stop interface.

## Requirements

1. Start profiling with a 5ms sampling interval.
2. Run a workload (a tight loop summing integers from 0 to 999999).
3. Stop profiling and capture the result.
4. Print the number of samples collected and the total profiling duration in milliseconds.
5. The profiler must raise an error if you attempt to start it while it is already running.
6. The profiler must raise an error if the sampling interval is less than 1ms.

## Test Cases

- Start the profiler, run a workload, stop it, and verify the returned result has a `sample_count` property that is a non-negative integer. [@test](./test_start_stop_basic.py)
- Attempt to call start twice without stopping; verify a `RuntimeError` is raised on the second call. [@test](./test_double_start_error.py)
- Attempt to call stop without calling start first; verify a `RuntimeError` is raised. [@test](./test_stop_without_start.py)
- Attempt to start with `interval_ms=0`; verify a `ValueError` is raised. [@test](./test_invalid_interval.py)

## Dependencies { .dependencies }

### spprof 0.1.0 { .dependency }

High-performance sampling profiler for Python with start/stop functional API.
