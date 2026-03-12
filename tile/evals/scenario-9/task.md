# Profile a Multi-Threaded Workload

Write a Python script that profiles a multi-threaded computation and analyzes the per-thread sample data from the resulting profile.

## Requirements

- Define a worker function `worker(n)` that computes the sum of `i * i` for `i` in range(n)
- Start the profiler with a 5ms interval before creating threads
- Create and start 3 worker threads, each computing `worker(500_000)`
- Wait for all threads to complete
- Stop the profiler
- From the profile, collect the set of unique `thread_id` values from all samples
- Print the number of unique threads observed: `"threads observed: <n>"`
- Print the total sample count: `"total samples: <n>"`

## Test Cases

- The profiler is started before threads are created @test
- All 3 threads are started and joined before stop() is called @test
- `profile.samples` contains `Sample` objects each with a `thread_id` attribute @test
- The number of unique thread IDs observed is printed as an integer @test

## Dependencies { .dependencies }

### spprof 0.1.0 { .dependency }

High-performance sampling profiler for Python applications. When started before spawning threads, automatically profiles all Python threads. The returned `Profile.samples` is a list of `Sample` objects, each with `thread_id`, `thread_name`, and `frames` attributes.
