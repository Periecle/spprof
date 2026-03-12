# Export Profile in Collapsed Stack Format

Write a Python script that profiles a computation and exports the result in collapsed stack format, which is compatible with FlameGraph tools such as Brendan Gregg's `flamegraph.pl`.

## Requirements

- Profile a workload using any profiling method
- Convert the profile to collapsed stack format using the appropriate method
- The collapsed format is a multi-line string where each line has the form: `frame1;frame2;...;frameN count`
- Save the profile to `"output.collapsed"` using the collapsed format
- Print the number of unique stack traces (lines) in the collapsed output

If the profile has no samples (e.g., the workload was too fast), print `0` for the line count.

## Test Cases

- `profile.to_collapsed()` returns a string @test
- `profile.save("output.collapsed", format="collapsed")` creates a file @test
- Each non-empty line in the collapsed output ends with a space and an integer count @test
- The number of unique stack traces is printed as an integer @test

## Dependencies { .dependencies }

### spprof 0.1.0 { .dependency }

High-performance sampling profiler for Python applications. The `Profile` object has a `to_collapsed()` method returning a collapsed stack string, and `save()` accepts `format="collapsed"` to write in FlameGraph-compatible format.
