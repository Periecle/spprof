# Feature Specification: Linux timer_create Robustness Improvements

**Feature Branch**: `001-linux-create-timer`  
**Created**: 2025-12-01  
**Status**: Draft  
**Input**: User description: "Instead of current itimer implementation in Linux profiler implement via create_timer API more robust profiling. Target only Linux changes, yet make coherent changes and small refactorings when needed."

## Context & Clarification

**Important Note**: The current Linux implementation (`src/spprof/_ext/platform/linux.c`) already uses the `timer_create()` API with `SIGEV_THREAD_ID` for per-thread CPU time sampling. The `setitimer` approach is only used on macOS (`darwin.c`).

This specification focuses on **improving the robustness** of the existing `timer_create` implementation on Linux, addressing current limitations:
- Fixed thread limit (256 threads maximum)
- No timer overrun compensation
- Race conditions during timer cleanup
- Inefficient thread tracking data structures
- Limited pause/resume capabilities

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Reliable Multi-Threaded Profiling (Priority: P1)

As a developer profiling a multi-threaded Python application, I want the profiler to reliably track all active threads without hitting artificial limits, so that I can profile applications with many concurrent threads.

**Why this priority**: Core functionality - users cannot profile applications exceeding the thread limit, leading to incomplete profiling data.

**Independent Test**: Profile a Python application that spawns 300+ worker threads and verify all threads are sampled proportionally to their CPU usage.

**Acceptance Scenarios**:

1. **Given** a Python application with 300 active threads, **When** profiling is started, **Then** all 300 threads should be registered and sampled.
2. **Given** profiling is active on a multi-threaded application, **When** threads are dynamically created and destroyed, **Then** thread tracking should handle registrations and unregistrations without memory leaks or crashes.
3. **Given** thread limit is reached (if any), **When** a new thread attempts to register, **Then** an appropriate warning is logged and profiling continues for registered threads.

---

### User Story 2 - Accurate Sampling Under High Load (Priority: P2)

As a developer profiling a CPU-intensive application, I want the profiler to compensate for timer overruns, so that my profiling data accurately reflects actual CPU usage patterns even when the system is under heavy load.

**Why this priority**: Profiling accuracy is critical for identifying performance bottlenecks - missed samples distort the profiling results.

**Independent Test**: Profile a CPU-intensive application and compare actual sample counts against expected counts based on configured interval and runtime.

**Acceptance Scenarios**:

1. **Given** a profiling interval of 10ms and a CPU-bound workload, **When** timer overruns occur due to system load, **Then** the profiler should report overrun statistics accurately.
2. **Given** timer overruns are detected, **When** querying profiling statistics, **Then** the user should see the overrun count and can factor this into their analysis.
3. **Given** high system load causing frequent overruns, **When** profiling completes, **Then** captured samples should be timestamped accurately relative to when they were actually captured.

---

### User Story 3 - Clean Profiler Shutdown (Priority: P2)

As a developer using the profiler in a production-like environment, I want profiling to start and stop cleanly without race conditions, so that my application doesn't crash or hang during profiler shutdown.

**Why this priority**: Stability is essential for adoption - crashes during shutdown damage user confidence and can corrupt profiling data.

**Independent Test**: Start and stop the profiler rapidly in a loop (1000 iterations per SC-003) while threads are actively being created and destroyed, and verify no crashes or hangs occur.

**Acceptance Scenarios**:

1. **Given** profiling is active with multiple thread timers, **When** profiling is stopped, **Then** all timers should be deleted without signal delivery races.
2. **Given** a thread is being unregistered while profiling stops, **When** both operations complete, **Then** no use-after-free or double-free errors occur.
3. **Given** profiling has been stopped, **When** it is restarted, **Then** a fresh state is established without leaking resources from the previous session.

---

### User Story 4 - Pause and Resume Profiling (Priority: P3)

As a developer analyzing specific code sections, I want to pause and resume profiling without destroying timers, so that I can focus sampling on code regions of interest.

**Why this priority**: Nice-to-have optimization for advanced use cases - allows more targeted profiling.

**Independent Test**: Pause profiling, execute code, resume profiling, and verify no samples were captured during the pause period.

**Acceptance Scenarios**:

1. **Given** profiling is active, **When** the user requests a pause, **Then** sample collection stops immediately but timers remain configured.
2. **Given** profiling is paused, **When** the user requests resume, **Then** sample collection continues without needing to recreate timers.
3. **Given** profiling is paused for an extended period, **When** it is resumed, **Then** there is no backlog of samples from the paused period.

---

### Edge Cases

- What happens when a thread exits while its timer is firing?
- How does the system behave when SIGPROF is blocked by the application?
- What happens if `timer_create` fails due to system resource limits (e.g., `RLIMIT_SIGPENDING`)?
- How does profiling behave when threads are rapidly created and destroyed (thread pool churn)?
- What happens if the ring buffer is full when a sample arrives?

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: System MUST dynamically allocate thread tracking storage, removing the fixed 256-thread limit.
- **FR-002**: System MUST track and expose timer overrun counts via the statistics API.
- **FR-003**: System MUST safely clean up timers during shutdown without race conditions between timer deletion and signal delivery.
- **FR-004**: System MUST support thread registration and unregistration at any time during the profiling session.
- **FR-005**: System MUST handle thread exit gracefully, cleaning up the associated timer.
- **FR-006**: System SHOULD support pausing and resuming profiling without recreating timers.
- **FR-007**: System MUST report meaningful error messages when timer creation fails (e.g., resource limits).
- **FR-008**: System MUST preserve existing platform API compatibility (`platform.h` interface).
- **FR-009**: System MUST use efficient data structures for thread lookup (better than O(n) linear search).
- **FR-010**: System MUST handle `timer_create` failures gracefully without crashing.

### Key Entities *(include if feature involves data)*

- **ThreadTimer**: Represents a per-thread timer with its timer_t handle, thread ID, and active status.
- **ThreadTimerRegistry**: Collection of active thread timers with efficient lookup and thread-safe operations.
- **TimerStatistics**: Aggregated statistics including samples captured, samples dropped, and timer overruns.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Applications with 500+ threads (minimum) can be profiled without hitting thread limits. Target: 1000+ concurrent threads.
- **SC-002**: Timer overrun counts are accurately reported and match kernel-level `timer_getoverrun` values.
- **SC-003**: 1000 start/stop cycles complete without memory leaks (verified via valgrind or similar).
- **SC-004**: Thread registration/unregistration operates correctly under concurrent access from 100+ threads.
- **SC-005**: Profiler shutdown completes within 100ms under normal conditions.
- **SC-006**: No crashes or hangs observed during stress testing with rapid thread creation/destruction.
- **SC-007**: Existing tests continue to pass, demonstrating backward compatibility.

## Assumptions

- The target system is Linux with kernel support for `timer_create` with `SIGEV_THREAD_ID` (Linux 2.6+).
- `CLOCK_THREAD_CPUTIME_ID` is available and provides per-thread CPU time measurement.
- Thread IDs (obtained via `gettid()` or `syscall(SYS_gettid)`) are unique within the process lifetime.
- The existing ring buffer implementation is sufficient and does not need modification.
- macOS (`darwin.c`) and Windows (`windows.c`) implementations are out of scope for this feature.
- The signal handler (`signal_handler.c`) may receive minor coherent updates but is not the focus.

## Out of Scope

- Changes to macOS `setitimer` implementation
- Changes to Windows timer queue implementation
- Ring buffer modifications
- Python API changes (unless needed for new statistics)
- New profiling features beyond robustness improvements
