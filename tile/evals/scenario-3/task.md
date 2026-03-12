# Export Profile in Speedscope Format

Write a Python script that profiles a computation and exports the result in Speedscope JSON format. The Speedscope format is the default output format and enables interactive flame graph visualization.

## Requirements

- Profile a workload using any profiling method (start/stop, context manager, or decorator)
- Convert the profile to a Speedscope-compatible Python dictionary using the appropriate method
- Verify the resulting dictionary contains the required Speedscope top-level keys: `"$schema"`, `"version"`, `"shared"`, and `"profiles"`
- Save the profile to `"output.json"` using the default format (Speedscope)
- Print the number of thread profiles present in the `"profiles"` list

## Test Cases

- The Speedscope dict has a `"$schema"` key @test
- The dict has a `"shared"` key containing a `"frames"` list @test
- `profile.save("output.json")` writes a valid JSON file with the Speedscope schema key @test
- The `"profiles"` list length is printed as an integer @test

## Dependencies { .dependencies }

### spprof 0.1.0 { .dependency }

High-performance sampling profiler for Python applications. The `Profile` object has a `to_speedscope()` method that returns a Speedscope-format dictionary, and a `save()` method that writes Speedscope JSON by default.
