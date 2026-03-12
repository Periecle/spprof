# Handle Profiler Errors and Validate Input

Write a Python script that demonstrates proper error handling for the sampling profiler's error conditions.

## Requirements

Implement a function `safe_profile(interval_ms)` that:
1. Validates that `interval_ms >= 1`; if not, catches the appropriate error and prints `"Error: invalid interval"`
2. Starts the profiler with the given interval
3. Attempts to start the profiler a second time; catches the appropriate error and prints `"Error: already running"`
4. Runs a short workload
5. Stops the profiler
6. Attempts to stop the profiler again; catches the appropriate error and prints `"Error: not running"`
7. Returns the profile from the first stop

Call `safe_profile(10)` and also call `safe_profile(0)` to demonstrate invalid interval handling.

## Test Cases

- `safe_profile(0)` catches the error raised when `interval_ms=0` and prints `"Error: invalid interval"` @test
- `safe_profile(10)`: attempting to start while running raises an error caught and prints `"Error: already running"` @test
- `safe_profile(10)`: attempting to stop twice raises an error caught and prints `"Error: not running"` @test
- `safe_profile(10)` returns a valid profile object @test

## Dependencies { .dependencies }

### spprof 0.1.0 { .dependency }

High-performance sampling profiler for Python applications. `start()` raises `ValueError` when `interval_ms < 1`, `RuntimeError` when already running, and `stop()` raises `RuntimeError` when not running.
