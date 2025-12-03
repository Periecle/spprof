# Feature Specification: Memory Allocation Profiler

**Feature Branch**: `006-memory-profiler`  
**Created**: December 3, 2024  
**Status**: Draft  
**Input**: User description: "Cover full memory profiler specification - production-grade, ultra-low-overhead memory profiling subsystem using Poisson sampling via native allocator interposition"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Basic Memory Profiling Session (Priority: P1)

As a Python developer, I want to profile my application's memory allocations so that I can identify which parts of my code are consuming the most memory and optimize accordingly.

**Why this priority**: This is the core use case - without basic profiling, no other features matter. Developers need to see where memory is being allocated to make optimization decisions.

**Independent Test**: Can be fully tested by starting the profiler, running a workload, capturing a snapshot, and verifying allocation sites appear with estimated memory usage.

**Acceptance Scenarios**:

1. **Given** a running Python application, **When** I start the memory profiler with default settings, **Then** the profiler begins tracking allocations with less than 0.1% CPU overhead.

2. **Given** the profiler is running, **When** my application allocates memory (Python objects, NumPy arrays, etc.), **Then** allocations are sampled and tracked with statistically accurate heap estimation.

3. **Given** allocations have been sampled, **When** I request a snapshot, **Then** I receive a summary showing estimated heap size, live samples, and top allocation sites with function names and file locations.

4. **Given** I have captured a snapshot, **When** I examine the top allocators, **Then** I can see Python function names, file paths, and line numbers for allocation sites.

---

### User Story 2 - Native Extension Visibility (Priority: P1)

As a data scientist using NumPy, PyTorch, or other C extensions, I want to see memory allocations from native code so that I can understand the full memory footprint of my application.

**Why this priority**: Python applications heavily rely on C extensions for performance. Without native visibility, most memory usage would be invisible to developers.

**Independent Test**: Can be tested by profiling a NumPy array creation and verifying the allocation site shows NumPy-related information.

**Acceptance Scenarios**:

1. **Given** the profiler is running, **When** my code calls NumPy to create a large array, **Then** the allocation is captured and attributed to the NumPy call site.

2. **Given** allocations from C extensions are captured, **When** I view the snapshot, **Then** I see both Python frames (my script) and native frames (the C extension) in the call stack.

3. **Given** C extensions are compiled with frame pointers, **When** viewing allocation stacks, **Then** the full call chain from Python through native code is visible.

---

### User Story 3 - Production-Safe Continuous Profiling (Priority: P1)

As a DevOps engineer, I want to run the memory profiler continuously in production so that I can detect memory issues without significantly impacting application performance.

**Why this priority**: Memory issues often only appear in production under real load. The profiler must be safe to run continuously without degrading service quality.

**Independent Test**: Can be tested by running a high-allocation-rate benchmark and measuring CPU overhead remains below 0.1%.

**Acceptance Scenarios**:

1. **Given** a production application processing 100+ MB/s of allocations, **When** the profiler is enabled with default settings, **Then** CPU overhead remains below 0.1%.

2. **Given** the profiler is running in production, **When** the application has been running for weeks, **Then** the profiler continues to operate correctly without memory leaks or degradation.

3. **Given** multiple threads are allocating concurrently, **When** the profiler is active, **Then** all threads are profiled correctly without contention or deadlocks.

---

### User Story 4 - Context Manager for Scoped Profiling (Priority: P2)

As a developer, I want to profile specific code sections using a context manager so that I can focus on particular workloads without noise from other parts of my application.

**Why this priority**: Targeted profiling is valuable but builds on the core profiling capability. Developers often want to isolate specific operations.

**Independent Test**: Can be tested by profiling a code block using `with` statement and verifying only allocations within that block are captured.

**Acceptance Scenarios**:

1. **Given** I wrap code in a profiling context manager, **When** the block executes, **Then** only allocations within that block are captured in the resulting snapshot.

2. **Given** I have completed a context manager block, **When** I access the snapshot property, **Then** I can save it to a file for later analysis.

---

### User Story 5 - Combined CPU and Memory Profiling (Priority: P2)

As a performance engineer, I want to run both CPU and memory profilers simultaneously so that I can correlate CPU hotspots with memory allocation patterns.

**Why this priority**: Understanding the relationship between CPU time and memory allocations provides deeper insights, but requires both profilers to work independently first.

**Independent Test**: Can be tested by starting both profilers, running a workload, and capturing both profiles independently.

**Acceptance Scenarios**:

1. **Given** I want comprehensive profiling, **When** I start both CPU and memory profilers, **Then** both operate correctly without interference.

2. **Given** both profilers are running, **When** I stop them and collect results, **Then** I get separate CPU profile and memory snapshot outputs.

---

### User Story 6 - Snapshot Export for Analysis Tools (Priority: P2)

As a developer, I want to export memory snapshots in standard formats so that I can analyze them in visualization tools like Speedscope.

**Why this priority**: Integration with existing analysis tools maximizes the value of captured data without requiring custom tooling.

**Independent Test**: Can be tested by exporting a snapshot to Speedscope format and opening it in the Speedscope web viewer.

**Acceptance Scenarios**:

1. **Given** I have a memory snapshot, **When** I save it with Speedscope format, **Then** the file can be loaded in Speedscope for visualization.

2. **Given** I save a snapshot, **When** I specify a file path, **Then** the snapshot is written to that path in the requested format.

---

### User Story 7 - Allocation Lifetime Tracking (Priority: P3)

As a developer investigating memory leaks, I want to see how long allocations remain live so that I can identify objects that are never freed.

**Why this priority**: Lifetime information is valuable for leak detection but builds on top of basic allocation tracking.

**Independent Test**: Can be tested by allocating objects, freeing some, taking a snapshot, and verifying freed allocations show lifetime duration while live ones do not.

**Acceptance Scenarios**:

1. **Given** allocations have been made and some freed, **When** I take a snapshot, **Then** live allocations show no lifetime (still active) while freed ones show duration.

2. **Given** I'm profiling over time, **When** I request statistics, **Then** I can see counts of total samples, live samples, and freed samples.

---

### User Story 8 - Profiler Statistics and Diagnostics (Priority: P3)

As a developer, I want to access profiler statistics so that I can understand the profiler's behavior and data quality.

**Why this priority**: Diagnostics help users understand if they have enough samples for statistical accuracy and detect any issues with the profiling configuration.

**Independent Test**: Can be tested by getting stats and verifying metrics like sample count, heap estimate, and load factor are reported.

**Acceptance Scenarios**:

1. **Given** the profiler has been running, **When** I request statistics, **Then** I receive sample counts, estimated heap size, unique stacks, and internal metrics.

2. **Given** I'm unsure about data quality, **When** I check statistics, **Then** I can see if enough samples were collected for statistically meaningful results.

---

### Edge Cases

- What happens when allocation rate is extremely high (millions per second)?
  - System continues to function with graceful degradation; some samples may be dropped but profiler remains stable.

- How does the system handle very small allocations that may rarely get sampled?
  - Small allocations are sampled proportionally less often (by design); users should understand this via documentation.

- What happens when the profiler runs out of internal storage capacity?
  - New samples are dropped gracefully without crashing; statistics indicate capacity issues.

- How does the system behave when process forks (multiprocessing)?
  - Profiler auto-disables in child processes to prevent corruption; users should use spawn start method for best results.

- What happens if C extensions lack frame pointers?
  - Stack traces are truncated at that point; warnings are emitted and statistics track truncation rate.

- How does the system handle allocations made during profiler startup/shutdown?
  - Re-entrancy guards prevent infinite recursion; bootstrap mechanism handles initialization-time allocations.

## Requirements *(mandatory)*

### Functional Requirements

**Core Profiling:**

- **FR-001**: System MUST capture memory allocations from Python code, C extensions, and native libraries.
- **FR-002**: System MUST use statistical sampling to estimate total heap usage with bounded error.
- **FR-003**: System MUST track both live (unfreed) allocations and freed allocations with lifetime duration.
- **FR-004**: System MUST capture call stacks for sampled allocations, including both Python and native frames.
- **FR-005**: System MUST operate with less than 0.1% CPU overhead at default settings.

**Sampling Configuration:**

- **FR-006**: System MUST allow configurable sampling rate (average bytes between samples).
- **FR-007**: System MUST use unbiased sampling where larger allocations are proportionally more likely to be sampled.
- **FR-008**: System MUST provide default sampling rate of 512 KB for production use.

**Snapshot and Reporting:**

- **FR-009**: System MUST provide snapshots of currently live (unfreed) sampled allocations.
- **FR-010**: System MUST provide estimated heap size based on statistical sampling.
- **FR-011**: System MUST report top allocation sites ranked by estimated memory usage.
- **FR-012**: System MUST resolve native addresses to function names, file names, and line numbers.
- **FR-013**: System MUST support exporting snapshots in Speedscope-compatible format.

**API and Integration:**

- **FR-014**: System MUST provide a Python API with start(), stop(), get_snapshot(), get_stats(), and shutdown() functions.
- **FR-015**: System MUST provide a context manager for scoped profiling.
- **FR-016**: System MUST operate independently from the CPU profiler (both can run simultaneously).

**Safety and Correctness:**

- **FR-017**: System MUST be thread-safe for concurrent allocations from multiple threads.
- **FR-018**: System MUST handle re-entrant allocations (allocations made by the profiler itself).
- **FR-019**: System MUST not crash or corrupt data when allocations are freed rapidly or out of order.
- **FR-020**: System MUST gracefully degrade when internal capacity is reached (drop samples, don't crash).

**Platform Support:**

- **FR-021**: System MUST support macOS via malloc_logger callback mechanism.
- **FR-022**: System MUST support Linux via LD_PRELOAD library mechanism.
- **FR-023**: System SHOULD support Windows (experimental, with documented limitations).

**Lifecycle Management:**

- **FR-024**: System MUST continue tracking frees after stop() to prevent false leak reports.
- **FR-025**: System MUST provide shutdown() for clean process exit.
- **FR-026**: System MUST handle process fork safely (auto-disable in children).

### Key Entities

- **AllocationSample**: A single sampled memory allocation with address, size, estimated weight, timestamp, lifetime (if freed), and call stack.

- **StackFrame**: A frame in the allocation call stack containing address, function name, file name, line number, and whether it's a Python or native frame.

- **HeapSnapshot**: A point-in-time view of all live sampled allocations, including total samples, live sample count, estimated heap bytes, and frame pointer health metrics.

- **MemProfStats**: Profiler operational statistics including total samples, live samples, freed samples, unique stacks, estimated heap, and internal metrics like collision counts.

- **FramePointerHealth**: Metrics for assessing native stack capture quality, including truncation rate and confidence level (high/medium/low).

## Success Criteria *(mandatory)*

### Measurable Outcomes

**Performance:**

- **SC-001**: Default profiling overhead is less than 0.1% CPU under typical Python workloads (100+ MB/s allocation rate).
- **SC-002**: Profiling overhead scales linearly with sampling rate - 64 KB rate yields less than 1% overhead.
- **SC-003**: Memory footprint is bounded (less than 60 MB) regardless of profiling duration.

**Accuracy:**

- **SC-004**: Heap size estimates are within 20% of actual values with 95% confidence given sufficient samples (1000+).
- **SC-005**: Top allocation sites by memory usage are correctly identified and ranked.
- **SC-006**: Python function names, file names, and line numbers are correctly resolved for allocation sites.

**Usability:**

- **SC-007**: Developers can start profiling, run a workload, and view results with less than 10 lines of Python code.
- **SC-008**: Profiler output is compatible with Speedscope visualization tool.
- **SC-009**: Clear warnings and documentation are provided when data quality may be affected (low sample count, missing frame pointers).

**Reliability:**

- **SC-010**: Profiler operates correctly for weeks of continuous production use without degradation.
- **SC-011**: No crashes, deadlocks, or data corruption under high concurrency (10+ threads allocating simultaneously).
- **SC-012**: Graceful degradation when internal limits are reached (samples dropped, not crashed).

**Coverage:**

- **SC-013**: Memory allocations from Python objects, NumPy arrays, PyTorch tensors, and other C extensions are captured.
- **SC-014**: Both Python and native frames appear in call stacks when frame pointers are available.

## Assumptions

- Python applications primarily allocate memory through malloc/free (directly or via C extensions).
- C extensions compiled with frame pointers will provide complete native stack traces.
- Users accept statistical estimates rather than exact byte counts for production-safe overhead.
- Standard web/mobile application performance expectations apply unless otherwise specified.
- Users have basic familiarity with profiling concepts and can interpret statistical results.

## Scope Boundaries

**In Scope:**

- Heap allocations via malloc/calloc/realloc/free
- Python object allocations (via PyMem which uses malloc)
- C extension allocations
- Statistical estimation with configurable sampling rate
- Call stack capture with mixed Python/native frames
- Export to standard visualization formats

**Out of Scope:**

- Exact byte-level memory tracking (we sample, not count)
- Python garbage collector integration (we intercept malloc, not GC)
- Memory leak detection algorithms (we provide data; analysis is separate)
- Real-time alerting (we collect data; alerting is separate concern)
- Direct mmap() calls that bypass malloc
- Memory-mapped files and regions
- Physical memory (RSS) vs virtual memory distinction (we track virtual)

## Dependencies

- Existing spprof CPU profiler infrastructure (framewalker, resolver, output formats)
- Platform-specific interposition mechanisms (malloc_logger on macOS, LD_PRELOAD on Linux)
- C compiler with frame pointer support for full stack traces
