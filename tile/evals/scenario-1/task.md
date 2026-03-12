# Profile a Code Block with a Context Manager

Use a sampling profiler library to profile a block of code using its context manager interface.

## Requirements

1. Profile a workload (a tight loop computing a sum) using the context manager form of the profiler, with a 2ms sampling interval.
2. After the `with` block exits, access the profile result from the context manager object and print the number of samples collected.
3. Verify that the profile property is `None` while profiling is still active (i.e., inside the `with` block) and is not `None` after exiting.
4. Configure the context manager to automatically save the profile to a file named `output.json` when the context exits.

## Test Cases

- Use the profiler context manager with interval_ms=2, profile a workload, then verify the profile result is accessible and has a non-negative sample_count. [@test](./test_context_manager_basic.py)
- Verify that the profile property is None while inside the `with` block. [@test](./test_profile_none_during.py)
- Configure output_path on the context manager, run a workload, and verify the file is created on disk after the context exits. [@test](./test_context_manager_autosave.py)

## Dependencies { .dependencies }

### spprof 0.1.0 { .dependency }

High-performance sampling profiler for Python with a context manager interface.
