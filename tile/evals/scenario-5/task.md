# Inspect and Traverse the Profile Data Model

Use a sampling profiler library to profile a workload and then traverse the resulting profile data structure to extract and print meaningful information.

## Requirements

1. Profile a CPU-intensive workload (a recursive Fibonacci function `fib(30)`) using a 1ms sampling interval.
2. After stopping, print the following from the Profile object:
   - `sample_count`: total number of samples
   - `total_duration_ms`: profiling duration in milliseconds
   - `effective_rate_hz`: effective sampling rate in Hz
   - `dropped_count`: number of dropped samples
   - `python_version` and `platform` metadata strings
3. Inspect the first sample (if any) and print:
   - `thread_id` of the sample
   - The number of frames in the sample
   - The `function_name`, `filename`, and `lineno` of the first (innermost) frame
4. Verify that `Frame.is_native` is a boolean attribute on each frame.

## Test Cases

- Profile a workload and verify profile.sample_count is a non-negative integer. [@test](./test_sample_count_type.py)
- Profile a workload and verify profile.effective_rate_hz is a positive float (greater than 0 when samples > 0). [@test](./test_effective_rate_hz.py)
- Profile a workload, access the first sample's frames list, and verify each Frame has function_name, filename, lineno, and is_native attributes. [@test](./test_frame_attributes.py)

## Dependencies { .dependencies }

### spprof 0.1.0 { .dependency }

High-performance sampling profiler for Python providing Profile, Sample, and Frame data classes.
