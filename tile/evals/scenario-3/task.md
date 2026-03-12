# Check Profiler Status and Live Statistics

Use a sampling profiler library to check whether profiling is active and retrieve live statistics during a profiling session.

## Requirements

1. Before starting profiling, verify that the profiler is not active and that requesting statistics returns `None`.
2. Start profiling with a 10ms interval.
3. After starting, verify the profiler is reported as active.
4. Run a workload (a tight loop) and retrieve the live statistics object; verify it has numeric fields for sample count and duration.
5. Stop profiling; verify the profiler is no longer active.

## Test Cases

- Before calling start, verify the status check returns False (not active). [@test](./test_not_active_before_start.py)
- Call start(), then verify the status check returns True (active). [@test](./test_active_after_start.py)
- Call start(), run a workload, call stats(), and verify the result is not None and has a non-negative collected_samples field. [@test](./test_stats_during_profiling.py)
- Call stop() after profiling; verify the status check returns False. [@test](./test_not_active_after_stop.py)

## Dependencies { .dependencies }

### spprof 0.1.0 { .dependency }

High-performance sampling profiler for Python with status check and live statistics API.
