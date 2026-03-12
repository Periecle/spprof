# Inspect Profile Properties and Metadata

Write a Python script that profiles a workload and then reads and prints multiple properties from the resulting profile object to produce a summary report.

## Requirements

- Profile a CPU-intensive workload for at least 200ms (use a loop with enough iterations or a sleep if needed)
- After profiling stops, print a formatted report containing:
  1. Number of samples collected (`sample_count`)
  2. Total profiling duration in milliseconds, rounded to 1 decimal place (`total_duration_ms`)
  3. Effective sampling rate in Hz, rounded to 1 decimal place (`effective_rate_hz`)
  4. Number of dropped samples (`dropped_count`)
  5. Python version string (`python_version`)
  6. Platform string (`platform`)

Output format (one value per line, label followed by colon and value):
```
samples: <int>
duration_ms: <float>
rate_hz: <float>
dropped: <int>
python_version: <str>
platform: <str>
```

## Test Cases

- `sample_count` is a non-negative integer @test
- `total_duration_ms` is greater than 0 @test
- `dropped_count` is a non-negative integer @test
- `python_version` is a non-empty string @test

## Dependencies { .dependencies }

### spprof 0.1.0 { .dependency }

High-performance sampling profiler for Python applications. The `Profile` object exposes `sample_count`, `total_duration_ms`, `effective_rate_hz` as computed properties, and `dropped_count`, `python_version`, `platform` as data fields.
