# Auto-Save Profile to File on Stop

Write a Python script that configures the profiler to automatically save the profile to a file when profiling stops, without making a separate explicit save call.

## Requirements

- Create a subdirectory `profiles/run1/`
- Start the profiler with `interval_ms=10` and configure it to auto-save to `"profiles/run1/result.json"` when stopped
- Run a workload of at least 100,000 iterations
- Stop the profiler (the profile should be automatically saved at this point)
- Verify that `profiles/run1/result.json` exists after stopping
- Print `"Saved"` if the file exists, `"Missing"` otherwise

## Test Cases

- After `stop()`, `profiles/run1/result.json` must exist @test
- No explicit `profile.save(...)` call should be needed @test
- The subdirectory `profiles/run1/` is created automatically if it doesn't exist @test
- The returned profile object from `stop()` is valid (has `sample_count` attribute) @test

## Dependencies { .dependencies }

### spprof 0.1.0 { .dependency }

High-performance sampling profiler for Python applications. The `start()` function and `Profiler` class accept an `output_path` argument; when set, the profile is automatically saved to that path when profiling stops.
