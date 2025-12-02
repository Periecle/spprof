# Feature Specification: Linux Free-Threading Support via Speculative Sampling

**Feature Branch**: `005-linux-freethreading`  
**Created**: December 2, 2024  
**Status**: Draft  
**Input**: User description: "Implement free-threading on Python 3.13 and 3.14 on Linux using Speculative Reading with Validation approach"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Profile Free-Threaded Python Application on Linux (Priority: P1)

A developer running Python 3.13t or 3.14t (free-threaded builds) on Linux wants to profile their application using spprof. Currently, this fails with an error message stating that signal-based sampling is unsafe for free-threaded builds. With this feature, the profiler captures stack samples speculatively, validates each frame pointer before use, and gracefully drops any corrupted samples.

**Why this priority**: This is the core functionality requested. Without this, users cannot profile free-threaded Python applications on Linux at all.

**Independent Test**: Can be fully tested by profiling a simple Python script on Linux with Python 3.13t/3.14t and verifying that samples are captured successfully.

**Acceptance Scenarios**:

1. **Given** a Linux system with Python 3.13t or 3.14t (free-threaded), **When** the user starts spprof profiling on a Python script, **Then** the profiler captures stack samples without crashing
2. **Given** a free-threaded Python application running multiple threads concurrently, **When** SIGPROF fires during frame chain modification, **Then** the sample is safely dropped (not a crash) and profiling continues
3. **Given** valid profiling session on free-threaded Python, **When** profiling completes, **Then** the output contains resolved function names and line numbers

---

### User Story 2 - View Drop Rate Statistics (Priority: P2)

A developer wants to understand how many samples were dropped due to validation failures during speculative capture, so they can assess profiling accuracy.

**Why this priority**: Important for user confidence in profiling results, but not required for basic functionality.

**Independent Test**: Can be tested by checking profiler statistics after a profiling session shows dropped sample count.

**Acceptance Scenarios**:

1. **Given** a completed profiling session on free-threaded Python, **When** the user queries profiler statistics, **Then** the system reports the number of captured samples and dropped samples separately
2. **Given** a profiling session with concurrent thread activity, **When** validation catches corrupted frames, **Then** the drop counter increments and the sample is discarded gracefully

---

### User Story 3 - ARM64 Linux Support (Priority: P2)

A developer running free-threaded Python on ARM64 Linux (e.g., AWS Graviton, Apple Silicon VMs, Raspberry Pi) wants to profile their application with the same reliability as x86-64 users.

**Why this priority**: ARM64 is increasingly common in cloud and embedded environments. The memory model differs from x86-64, requiring memory barriers for safe speculative reads.

**Independent Test**: Can be tested by running the profiler on ARM64 Linux with free-threaded Python and verifying samples are captured correctly.

**Acceptance Scenarios**:

1. **Given** an ARM64 Linux system with Python 3.13t or 3.14t, **When** the user profiles an application, **Then** the profiler uses appropriate memory barriers for safe speculative reads
2. **Given** ARM64's weaker memory model, **When** reading frame chain pointers, **Then** acquire barriers ensure visibility of previously written values

---

### Edge Cases

- What happens when the frame chain forms a cycle due to corruption?
  - Cycle detection identifies the loop and terminates frame walking, dropping the sample
- What happens when a frame pointer points to freed/invalid memory?
  - Pointer validation (heap bounds check, alignment) catches obviously invalid addresses and bails early
- What happens when the code object type check fails?
  - The sample drops the corrupted frame but continues walking if previous frames are valid
- What happens on a 32-bit Linux system?
  - The feature supports 64-bit systems only (x86-64 and ARM64); 32-bit builds fall back to disabled state with clear error message

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: System MUST support speculative frame capture on Linux with Python 3.13t and 3.14t (free-threaded builds)
- **FR-002**: System MUST validate each frame pointer before dereferencing (heap bounds, alignment check)
- **FR-003**: System MUST detect frame chain cycles using a rolling window of recently seen addresses
- **FR-004**: System MUST validate code object pointers by comparing ob_type to cached PyCode_Type
- **FR-005**: System MUST use memory acquire barriers on ARM64 for all frame pointer reads
- **FR-006**: System MUST gracefully drop samples that fail validation (no crash, increment counter)
- **FR-007**: System MUST cache PyCode_Type pointer at initialization time (not in signal handler)
- **FR-008**: System MUST handle tagged pointers in Python 3.14's _PyStackRef correctly
- **FR-009**: System MUST maintain separate counters for captured samples and dropped samples
- **FR-010**: System MUST remain async-signal-safe throughout the speculative capture path

### Key Entities

- **SpeculativeCapture**: The new frame capture function that performs validation during frame walking
- **ValidationState**: Cached state including PyCode_Type pointer and heap bounds, initialized once at startup
- **CycleDetector**: Rolling window of recently visited frame addresses to detect circular chains
- **SampleStatistics**: Counters tracking captured vs. dropped samples for user visibility

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Developers can profile free-threaded Python applications on Linux without crashes or hangs
- **SC-002**: At least 99% of samples are successfully captured under normal workloads (low contention)
- **SC-003**: Zero crashes occur when profiling applications with high thread contention
- **SC-004**: Users can view the ratio of captured to dropped samples after profiling
- **SC-005**: ARM64 Linux users have equivalent profiling capability to x86-64 users
- **SC-006**: Profiling overhead remains comparable to existing GIL-enabled profiling (within 2x)

### Assumptions

- The user has Python 3.13t or 3.14t installed (free-threaded build with `Py_GIL_DISABLED`)
- The application runs on 64-bit Linux (x86-64 or ARM64)
- Python's arena allocator provides sufficient memory stability that recently-freed frames remain readable briefly
- Pointer reads are atomic at the hardware level on both x86-64 and ARM64
- The profiling interval (default 10ms) provides adequate statistical sampling even with occasional dropped samples
