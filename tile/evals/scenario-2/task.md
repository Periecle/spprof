# Profile a Function Using a Decorator

Use a sampling profiler library to profile a function by applying it as a decorator.

## Requirements

1. Define a function `compute(n)` that returns the sum of integers from 0 to n-1.
2. Apply the profiler as a decorator to `compute`, using a 1ms sampling interval and saving the profile to `compute_profile.json`.
3. Call `compute(500000)` and verify the return value is correct (i.e., `n*(n-1)//2`).
4. Verify that the decorator preserves the original function's name (`compute`) and that the profile file is created after the function returns.

## Test Cases

- Apply the decorator and call the decorated function; verify the return value equals the expected sum. [@test](./test_decorator_return_value.py)
- Apply the decorator with output_path set; call the function, then verify the output file exists. [@test](./test_decorator_output_file.py)
- Verify that the decorated function's `__name__` attribute is `"compute"` (i.e., functools.wraps is used). [@test](./test_decorator_preserves_name.py)

## Dependencies { .dependencies }

### spprof 0.1.0 { .dependency }

High-performance sampling profiler for Python with a function decorator interface.
