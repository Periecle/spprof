# Export a Profile to Speedscope JSON Format

Use a sampling profiler library to profile a workload and export the resulting profile data to the Speedscope JSON format.

## Requirements

1. Profile a CPU-intensive workload using a 5ms sampling interval.
2. Export the profile to Speedscope JSON format in two ways:
   a. Using the method that returns a Python dictionary (do not save to disk), then verify the dictionary has `"$schema"` and `"profiles"` keys.
   b. Using the method that saves directly to a file named `profile.json`, then read the file back and verify it is valid JSON with the same keys.
3. Also export the profile using the aggregated form: call the aggregation method first, then export the aggregated result to Speedscope format and save to `profile_agg.json`.
4. Verify that `profile_agg.json` is valid JSON with `"$schema"` and `"profiles"` keys.

## Test Cases

- Profile a workload and call to_speedscope(); verify the result is a dict with a "$schema" key. [@test](./test_to_speedscope_dict.py)
- Profile a workload, call save("profile.json"), and verify the file exists and contains valid JSON with "profiles" key. [@test](./test_save_speedscope_file.py)
- Profile a workload, aggregate it, and save the aggregated profile to Speedscope format; verify the result file is valid JSON. [@test](./test_aggregated_speedscope.py)

## Dependencies { .dependencies }

### spprof 0.1.0 { .dependency }

High-performance sampling profiler for Python with Speedscope JSON export support.
