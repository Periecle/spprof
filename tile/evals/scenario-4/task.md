# Handle Profiler Error Conditions

Use a sampling profiler library and write code that demonstrates and correctly handles all documented error conditions.

## Requirements

Demonstrate the following four error conditions by writing code that triggers each and catches the expected exception type:

1. Starting the profiler with `interval_ms` set to a value less than 1 must raise a `ValueError`.
2. Starting the profiler when it is already running must raise a `RuntimeError`.
3. Stopping the profiler when it is not running must raise a `RuntimeError`.
4. Starting the profiler with an `output_path` pointing to a directory that cannot be created (e.g., a path inside a non-existent root like `/nonexistent_root_dir/profile.json` that will fail on write) must raise a `PermissionError`.

For each case, print a confirmation message indicating the error was caught.

## Test Cases

- Pass interval_ms=0 to start; confirm ValueError is raised. [@test](./test_invalid_interval_zero.py)
- Call start() twice in a row; confirm RuntimeError is raised on the second call. [@test](./test_double_start.py)
- Call stop() without calling start; confirm RuntimeError is raised. [@test](./test_stop_not_running.py)
- Call start() with an unwritable output path; confirm PermissionError is raised. [@test](./test_unwritable_path.py)

## Dependencies { .dependencies }

### spprof 0.1.0 { .dependency }

High-performance sampling profiler for Python that raises specific exceptions for misuse.
