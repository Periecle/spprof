# Feature Specification: Python Sampling Profiler

**Feature Branch**: `001-python-sampling-profiler`  
**Created**: 2025-11-29  
**Status**: Draft  
**Input**: User description: "High-performance, in-process sampling profiler for Python, inspired by the JVM's async-profiler. Must operate within a Restricted Kubernetes securityContext (no SYS_PTRACE, no CAP_PERFMON, no privileged mode). Target environment covers Python versions 3.9 through 3.14."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Profile Production Application (Priority: P1)

A developer wants to identify CPU hotspots in a Python application running in a Kubernetes pod with restricted security context. They need to attach the profiler, collect samples for a defined period, and generate a flame graph to visualize where time is spent.

**Why this priority**: This is the core use case - production profiling in constrained environments where traditional profilers fail due to missing privileges.

**Independent Test**: Can be fully tested by importing the profiler, starting collection, running a CPU-intensive workload, stopping collection, and verifying flame graph output exists.

**Acceptance Scenarios**:

1. **Given** a Python application running in a restricted K8s pod, **When** the developer imports and starts the profiler from within the application code, **Then** sampling begins without requiring elevated privileges.
2. **Given** the profiler is actively sampling, **When** the developer calls the stop function, **Then** profiling data is written to the specified output location.
3. **Given** profiling has completed, **When** the output file is opened in Speedscope, **Then** a valid flame graph is displayed showing Python function call stacks.

---

### User Story 2 - Profile Multi-threaded Worker Application (Priority: P2)

A developer needs to profile a Python application with multiple worker threads to understand which threads consume the most CPU and identify bottlenecks across the thread pool.

**Why this priority**: Multi-threaded applications are common in production; profiler must work correctly regardless of GIL state.

**Independent Test**: Can be tested by running a multi-threaded application, profiling all threads, and verifying each thread's samples appear in the output with correct thread identification.

**Acceptance Scenarios**:

1. **Given** a Python application with 5 worker threads, **When** profiling is active, **Then** samples are collected from all threads including the main thread.
2. **Given** collected samples from multiple threads, **When** viewing the flame graph, **Then** the user can distinguish samples by thread.
3. **Given** a thread is executing C extension code, **When** a sample is taken, **Then** the Python stack frames leading to the C call are captured.

---


### User Story 3 - Profile Across Python Versions (Priority: P3)

A library maintainer needs to profile their library running on multiple Python versions (3.9 through 3.14) to ensure consistent performance characteristics across the support matrix.

**Why this priority**: Version compatibility is essential for broad adoption; enables library authors to use the profiler.

**Independent Test**: Can be tested by running the same profiling script on each supported Python version and verifying output is generated successfully.

**Acceptance Scenarios**:

1. **Given** the profiler installed in a Python 3.9 environment, **When** profiling is started, **Then** samples are collected correctly.
2. **Given** the profiler installed in a Python 3.14 environment, **When** profiling is started, **Then** samples are collected correctly despite interpreter structure changes.
3. **Given** any supported Python version, **When** the profiler accesses interpreter frame data, **Then** the correct frame structure is used for that version.

---

### Edge Cases

- What happens when profiling starts but no activity occurs during the sampling period? The profiler should produce a valid output file with zero or minimal samples.
- How does the system handle when the output path is not writable? The profiler should fail gracefully with a clear error message before starting collection.
- What happens when the profiler is started twice without stopping? The second start call should either return an error or be a no-op.
- How does the system handle signal delivery when the main thread is blocked? Samples should still be collected from other threads.
- What happens when a Python thread terminates during profiling? The profiler should handle thread exit gracefully and continue profiling remaining threads.
- What happens when an existing SIGPROF handler is already installed? The profiler chains to the previous handler—capturing its sample first, then invoking the original handler.
- What happens when a call stack exceeds the maximum capture depth (128 frames)? The profiler truncates from the bottom, preserving the most recent (top) frames where the sample was taken.

## Requirements *(mandatory)*

### Functional Requirements

#### Core Profiling Capabilities

- **FR-001**: System MUST collect CPU samples using OS signal-based interrupts (SIGPROF) to minimize interpreter overhead.
- **FR-002**: System MUST capture Python interpreter frame stacks (_PyInterpreterFrame chain) when sampling, regardless of whether the interpreter is executing Python bytecode or C code.
- **FR-003**: System SHOULD capture native C stack frames alongside Python frames when the interpreter is executing C extension code, creating mixed-mode stacks (optional via libunwind on Linux).
- **FR-004**: System MUST support configurable sampling intervals with a default of 10ms and minimum of 1ms.
- **FR-005**: System MUST collect samples from all Python threads in the process, not just the calling thread.
- **FR-006**: System MUST function correctly regardless of the GIL state (held, released, or disabled in free-threaded builds).
- **FR-007**: System MUST record accurate wall-clock timestamps for each sample.
- **FR-008**: System MUST attribute samples to the correct thread with thread identification.

#### Environment Integration

- **FR-009**: System MUST operate without requiring SYS_PTRACE capability.
- **FR-010**: System MUST operate without requiring CAP_PERFMON capability.
- **FR-011**: System MUST operate without requiring privileged container mode.
- **FR-012**: System MUST function when running as a non-root user.
- **FR-013**: System MUST support writing output to a user-specified path, enabling use with read-only root filesystems and writable temp volumes.
- **FR-014**: System MUST be installable as a standard Python package via pip.
- **FR-015**: System MUST expose a Python API for starting and stopping profiling programmatically.
- **FR-016**: System MUST handle graceful cleanup if the process terminates while profiling is active.
- **FR-031**: System MUST expose operational statistics via Python API (collected samples, dropped samples, memory usage) without logging to stderr.

#### Data Output

- **FR-017**: System MUST produce output in a format compatible with Speedscope JSON specification for flame graph visualization.
- **FR-018**: System MUST support collapsed stack format compatible with FlameGraph CLI tools
- **FR-019**: System MUST include function names, file names, and line numbers in the stack frame data.
- **FR-020**: System MUST include thread identifiers in the output to enable per-thread analysis.
- **FR-021**: System MUST include timestamps or duration information to enable time-based analysis.
- **FR-022**: System MUST produce valid output files even when zero samples are collected.

#### Version Compatibility

- **FR-023**: System MUST support Python 3.9.x interpreters.
- **FR-024**: System MUST support Python 3.10.x interpreters.
- **FR-025**: System MUST support Python 3.11.x interpreters.
- **FR-026**: System MUST support Python 3.12.x interpreters.
- **FR-027**: System MUST support Python 3.13.x interpreters.
- **FR-028**: System MUST support Python 3.14.x interpreters (including pre-release versions).
- **FR-029**: System MUST correctly handle interpreter frame structure differences between Python versions.
- **FR-030**: System MUST support free-threaded Python builds (Python 3.13+ with GIL disabled).

### Key Entities

- **Sample**: A single captured snapshot containing timestamp, thread ID, and stack trace (list of frames). Maximum 128 frames per sample; deeper stacks are truncated from the bottom (oldest frames removed, most recent preserved).
- **Frame**: A single entry in a call stack, containing function name, file path, line number, and type (Python or native).
- **Profile**: A collection of samples gathered during a profiling session, with metadata about start/end times and configuration.
- **Thread**: A Python thread being profiled, identified by its thread ID and optional name.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Profiler overhead does not exceed 5% of application CPU time when sampling at 10ms intervals.
- **SC-002**: 100% of collected samples include complete Python stack traces with function names and line numbers.
- **SC-003**: Profiler successfully operates in Kubernetes pods with restricted security context (no elevated privileges).
- **SC-004**: Output files are correctly parsed and displayed by Speedscope without errors.
- **SC-005**: Profiler produces valid output across all supported Python versions (3.9 through 3.14).
- **SC-006**: Multi-threaded applications show samples from all active threads in the output.
- **SC-007**: Profiler starts and stops within 100ms of the API call.
- **SC-008**: Generated flame graphs display function names, file paths, and call hierarchy enabling hotspot identification.
- **SC-009**: Profiler memory usage does not exceed configured limit (default 100MB); configurable via API.

## Clarifications

### Session 2025-11-29

- Q: What behavior when existing SIGPROF handler detected? → A: Chain (capture sample, then call previous handler)
- Q: Stack depth truncation behavior? → A: Keep top (preserve most recent frames, truncate oldest)
- Q: Profiler self-observability? → A: Stats API only (expose metrics programmatically, no stderr logging)
- Q: Memory usage bound? → A: Configurable with 100MB default
- Q: FR-003 native C stack frames implementation? → A: Optional via libunwind (Linux), decoupled from MVP

## Assumptions

- The target application has sufficient CPU time for the profiler to collect meaningful samples.
- SIGPROF signal handling is available in the target environment (standard POSIX feature).
- The temporary volume mount (for output files) is provided by the Kubernetes pod configuration.
- The profiler will be embedded in the application code, not attached externally.
- Python debug symbols are not required; the profiler uses interpreter introspection.
- If an existing SIGPROF handler is installed, the profiler MUST chain to it (capture sample first, then invoke previous handler).
