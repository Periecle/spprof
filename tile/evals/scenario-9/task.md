# Conditionally Enable Native C-Stack Unwinding for Mixed-Mode Profiling

Use a sampling profiler library to check for native C-stack unwinding support and conditionally enable it before starting mixed-mode profiling.

## Requirements

1. Check whether native C-stack unwinding is available on the current platform.
2. If available, enable native unwinding and then verify it is enabled using the corresponding check function.
3. If not available, print a message indicating it is unavailable, and verify that attempting to enable it raises a `RuntimeError`.
4. Start profiling (with native unwinding enabled if available) using a 10ms interval, run a workload, and stop the profiler.
5. Check the collected frames: if any frames are marked as native (i.e., `Frame.is_native == True`), print the native frame's function name.
6. After profiling, disable native unwinding (if it was enabled) and verify it is now disabled.

## Test Cases

- Call the function that checks native unwinding availability and verify it returns a boolean. [@test](./test_native_available_bool.py)
- Attempt to enable native unwinding when it is not available (mock or skip on supported platforms); verify RuntimeError is raised. [@test](./test_enable_when_unavailable.py)
- If native unwinding is available, enable it and verify the enabled-check returns True; then disable it and verify the enabled-check returns False. [@test](./test_enable_disable_toggle.py)
- Profile a workload with native unwinding enabled (if available) and verify Frame objects have an is_native boolean attribute. [@test](./test_native_frames_attribute.py)

## Dependencies { .dependencies }

### spprof 0.1.0 { .dependency }

High-performance sampling profiler for Python with native C-stack unwinding support.
