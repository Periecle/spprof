# Feature Specification: Darwin Mach-Based Sampler

**Feature Branch**: `003-darwin-mach-sampler`  
**Created**: 2024-12-01  
**Status**: Draft  
**Input**: User description: "Implement Mach-based Suspend-Walk-Resume profiler for macOS using Mach kernel APIs to replace setitimer and achieve Windows-style precise thread sampling."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Profile Python Applications on macOS (Priority: P1)

As a Python developer on macOS, I want to profile my Python application to identify performance bottlenecks with accurate per-thread sampling data, so I can optimize my code effectively.

**Why this priority**: Core functionality - without accurate profiling data, the profiler has no value. The current setitimer approach delivers signals randomly (mostly to the main thread), making multi-threaded profiling unreliable.

**Independent Test**: Can be fully tested by running a multi-threaded Python application with the profiler enabled and verifying that samples are captured from all active threads, not just the main thread.

**Acceptance Scenarios**:

1. **Given** a multi-threaded Python application with 4 active threads, **When** the profiler runs for 10 seconds at 100Hz, **Then** samples are captured from all 4 threads (not just the main thread).

2. **Given** a CPU-bound Python function running on macOS, **When** the profiler captures samples, **Then** the resulting profile accurately identifies the hot functions consuming CPU time.

3. **Given** a profiler configured with 1ms sampling interval, **When** profiling a workload, **Then** the actual sampling rate is within 10% of the configured rate.

---

### User Story 2 - Profile Mixed Python/Native Code (Priority: P2)

As a developer using Python libraries with native C extensions (NumPy, PyTorch, etc.), I want to see both Python and native stack frames in my profile, so I can identify bottlenecks that occur in native code called from Python.

**Why this priority**: Mixed-mode profiling is essential for real-world Python applications that rely heavily on native extensions. Without this, developers miss critical performance insights.

**Independent Test**: Can be tested by profiling a Python application that calls NumPy operations and verifying both Python frames and native frames appear in the profile output.

**Acceptance Scenarios**:

1. **Given** Python code calling a CPU-intensive NumPy function, **When** the profiler captures samples, **Then** both the Python call site and the native NumPy function appear in the profile.

2. **Given** a profile with mixed Python/native stacks, **When** viewing the results, **Then** Python and native frames are correctly interleaved showing the actual call hierarchy.

---

### User Story 3 - Minimal Performance Impact (Priority: P2)

As a developer, I want profiling to have minimal overhead on my application, so I can profile in near-production conditions without significantly altering application behavior.

**Why this priority**: High profiling overhead distorts measurements and makes the profiler impractical for production use. The suspend-resume approach should be faster than signal-based sampling.

**Independent Test**: Can be tested by running the same workload with and without profiling, measuring total execution time and CPU usage.

**Acceptance Scenarios**:

1. **Given** a CPU-bound application running without profiling, **When** profiling is enabled at 100Hz, **Then** the application slowdown is less than 5%.

2. **Given** profiling enabled, **When** measuring the time each thread is suspended for sampling, **Then** the suspension time per sample is less than 100 microseconds.

---

### User Story 4 - Reliable Thread Discovery (Priority: P3)

As a developer using Python threading, asyncio, or GCD, I want all threads to be automatically discovered and profiled without manual registration, so I don't miss any thread's activity.

**Why this priority**: Automatic thread discovery ensures comprehensive profiling without developer intervention. Missing threads would lead to incomplete profiles.

**Independent Test**: Can be tested by spawning threads dynamically during profiling and verifying they are sampled.

**Acceptance Scenarios**:

1. **Given** a Python application that spawns new threads during execution, **When** profiling is active, **Then** newly created threads are automatically discovered and sampled.

2. **Given** threads that terminate during profiling, **When** a thread exits, **Then** the profiler handles the termination gracefully without errors or crashes.

3. **Given** an application using GCD (Grand Central Dispatch) worker threads, **When** profiling, **Then** GCD worker threads are included in the profile.

---

### User Story 5 - Architecture Support (Priority: P3)

As a developer on Apple Silicon (M1/M2/M3) or Intel Mac, I want the profiler to work correctly on my hardware, so I can profile regardless of my Mac's architecture.

**Why this priority**: macOS runs on both x86_64 and arm64. Supporting both ensures the profiler works for all macOS users.

**Independent Test**: Can be tested by building and running the profiler on both Intel and Apple Silicon Macs.

**Acceptance Scenarios**:

1. **Given** an Apple Silicon Mac (arm64), **When** the profiler runs, **Then** stack frames are correctly captured using arm64 register conventions.

2. **Given** an Intel Mac (x86_64), **When** the profiler runs, **Then** stack frames are correctly captured using x86_64 register conventions.

---

### Edge Cases

- What happens when the target thread is already suspended (e.g., by a debugger)?
  - **Expected**: Skip the thread for this sample cycle; do not increment suspend count further.
- How does the profiler behave when memory pressure causes stack pages to be paged out?
  - **Expected**: Stack walk terminates early if frame pointer validation fails; partial sample captured.
- What happens if a thread is terminated while the profiler is capturing its stack?
  - **Expected**: Handle KERN_TERMINATED gracefully; skip thread and continue sampling others.
- How does the profiler handle threads blocked on system calls?
  - **Expected**: Thread is sampled normally; system call appears as native frame.
- What happens when sampling frequency is set higher than the system can achieve?
  - **Expected**: Validate interval (minimum 1ms) and reject invalid configuration.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: Profiler MUST capture stack samples from all active Python threads, not just the main thread.
- **FR-002**: Profiler MUST support configurable sampling intervals from 1ms to 1000ms.
- **FR-003**: Profiler MUST automatically discover new threads as they are created without manual registration.
- **FR-004**: Profiler MUST handle thread termination gracefully without crashes or memory leaks.
- **FR-005**: Profiler MUST capture both Python stack frames and native (C/C++) stack frames when mixed-mode profiling is enabled.
- **FR-006**: Profiler MUST work on both x86_64 (Intel) and arm64 (Apple Silicon) architectures.
- **FR-007**: Profiler MUST resume suspended threads within 100 microseconds to prevent application hangs.
- **FR-008**: Profiler MUST provide statistics on samples captured, samples dropped, and profiler overhead.
- **FR-009**: Profiler MUST use high-precision timing for consistent sampling intervals.
- **FR-010**: Profiler MUST NOT require special entitlements or elevated privileges for profiling the current process.

### Key Entities

- **Sample**: A captured snapshot containing timestamp, thread identifier, and stack frames (Python and/or native).
- **Thread Registry**: Collection of active threads being tracked, updated as threads are created and destroyed.
- **Sampler**: Background worker that periodically suspends target threads, captures their state, and resumes them.
- **Stack Frame**: A single entry in the call stack, containing function/method identification and source location.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Multi-threaded applications receive samples from all active threads (not biased toward main thread), verified by sample distribution across threads within 20% of equal distribution for uniform workloads.
- **SC-002**: Sampling rate achieves 95% accuracy relative to the configured interval (e.g., 100Hz config yields 95-105 samples per second).
- **SC-003**: Application performance overhead is less than 5% at 100Hz sampling rate for CPU-bound workloads.
- **SC-004**: Per-sample thread suspension time is less than 100 microseconds in 99% of samples.
- **SC-005**: Zero crashes or memory leaks during continuous profiling sessions of 1+ hours.
- **SC-006**: New threads are discovered and sampled within 2 sampling intervals of their creation.
- **SC-007**: Profiler correctly captures stack frames on both Intel and Apple Silicon Macs without architecture-specific user configuration.
- **SC-008**: Mixed-mode profiles show correct interleaving of Python and native frames matching actual call hierarchy.

## Assumptions

- Frame pointers are reliably present on macOS, especially on Apple Silicon where the ABI enforces them.
- The profiler targets the current process only; profiling external processes (requiring `task_for_pid`) is out of scope.
- Python's GIL does not need to be held during the suspend/capture/resume cycle for native frame walking; GIL is only needed for resolving Python frame information.
- Stack memory is accessible for reading during thread suspension without requiring special memory protection handling in the common case.

## Out of Scope

- Profiling external processes (would require `task_for_pid` entitlement).
- Windows or Linux platform changes (this spec is Darwin-specific).
- Real-time profiling visualization (profiles are captured for post-hoc analysis).
- CPU time accounting (wall-clock time sampling only for this iteration).
