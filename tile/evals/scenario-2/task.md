# Profile a Function Using a Decorator

Write a Python script that uses the decorator form of a sampling profiler to automatically profile a function every time it is called.

## Requirements

- Define a function `compute(n)` that computes the sum of squares from 0 to n-1
- Apply the profiler decorator to `compute` with a 20ms interval and output path `"compute_profile.json"`
- Call `compute(500_000)`
- Verify that `compute_profile.json` is created after the function call
- The function must still return the correct numeric result (sum of squares)

The decorator must be applied using the `@decorator_name(...)` syntax above the function definition.

## Test Cases

- `compute(500_000)` returns the correct sum of squares `41666416666750000` @test
- After `compute(500_000)` is called, the file `compute_profile.json` exists @test
- The decorator must preserve the function's return value @test
- The decorator syntax must use `interval_ms` and `output_path` keyword arguments @test

## Dependencies { .dependencies }

### spprof 0.1.0 { .dependency }

High-performance sampling profiler for Python applications. Provides a `profile` decorator factory for profiling individual functions, accepting `interval_ms` and `output_path` arguments.
