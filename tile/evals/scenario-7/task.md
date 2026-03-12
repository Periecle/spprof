# Aggregate Profile Samples to Reduce Memory Usage

Use a sampling profiler library to profile a repetitive workload, then aggregate the resulting profile to reduce memory usage by merging identical call stacks.

## Requirements

1. Profile a repetitive workload (a loop calling `math.sqrt(x)` 1,000,000 times) using a 1ms sampling interval.
2. Stop profiling to get the raw Profile object.
3. Call the aggregation method on the Profile to get an AggregatedProfile.
4. From the AggregatedProfile, read and print:
   - `unique_stack_count`: number of distinct call stacks
   - `compression_ratio`: ratio of original sample count to unique stack count
   - `memory_reduction_pct`: estimated memory reduction percentage
   - `total_samples`: original number of samples before aggregation
5. Verify that `total_samples` on the AggregatedProfile equals `sample_count` on the original Profile.
6. Verify that the aggregated stacks list contains `AggregatedStack` items, each with `frames`, `thread_id`, `thread_name`, and `count` attributes.

## Test Cases

- Profile a repetitive workload, aggregate it, and verify agg.total_samples == profile.sample_count. [@test](./test_total_samples_match.py)
- Verify agg.unique_stack_count is a positive integer <= agg.total_samples. [@test](./test_unique_stack_count.py)
- Verify each item in agg.stacks has a count >= 1 and a frames list. [@test](./test_aggregated_stack_items.py)
- Verify agg.compression_ratio >= 1.0. [@test](./test_compression_ratio.py)

## Dependencies { .dependencies }

### spprof 0.1.0 { .dependency }

High-performance sampling profiler for Python with profile aggregation support.
