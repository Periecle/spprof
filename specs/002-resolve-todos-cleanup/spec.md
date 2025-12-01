# Feature Specification: Resolve TODOs, Race Conditions, and Incomplete Implementations

**Feature Branch**: `002-resolve-todos-cleanup`  
**Created**: 2025-12-01  
**Status**: Draft  
**Input**: User description: "Resolve all TODOs, race conditions, and incomplete implementations in the project"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Complete Statistics API (Priority: P1)

A developer profiling their application wants accurate statistics about the profiling session, including the actual number of dropped samples and estimated overhead percentage. Currently, these values are hardcoded to zero in the Python API, making it impossible to assess profiling quality.

**Why this priority**: This directly affects users' ability to trust profiling results. Without accurate dropped sample counts, users cannot know if they are missing significant data. Without overhead estimates, users cannot tune sampling intervals appropriately.

**Independent Test**: Can be tested by running a high-frequency profiling session (1ms interval) under load, calling `stats()`, and verifying that `dropped_samples` and `overhead_estimate_pct` return non-zero values when appropriate.

**Acceptance Scenarios**:

1. **Given** profiling is active with a 1ms interval under CPU load, **When** the user calls `spprof.stats()`, **Then** `dropped_samples` reflects the actual count from the native ring buffer.
2. **Given** profiling is active, **When** the user calls `spprof.stop()`, **Then** `Profile.dropped_count` contains the actual number of dropped samples.
3. **Given** profiling is active, **When** the user calls `spprof.stats()`, **Then** `overhead_estimate_pct` provides a reasonable estimate based on sampling frequency and duration.

---

### User Story 2 - Race-Free macOS Shutdown (Priority: P2)

A developer using the profiler on macOS wants reliable profiler shutdown without signal delivery race conditions. Currently, the macOS implementation uses a naive `nanosleep()` workaround instead of the proper signal blocking and draining approach used on Linux.

**Why this priority**: Race conditions during shutdown can cause crashes or hangs, particularly when start/stop is called frequently. The Linux implementation already solved this problem; macOS should follow the same pattern.

**Independent Test**: Can be tested by running `platform_timer_destroy()` 1000 times in rapid succession on macOS and verifying no crashes or hangs occur.

**Acceptance Scenarios**:

1. **Given** profiling is active on macOS, **When** profiling is stopped, **Then** all in-flight SIGPROF signals are properly drained.
2. **Given** a multi-threaded macOS application, **When** profiler start/stop is called 1000 times in a loop, **Then** no crashes, hangs, or signal-related errors occur.
3. **Given** macOS implementation, **When** `platform_timer_destroy()` is called, **Then** signal blocking and `sigtimedwait()` are used instead of `nanosleep()`.

---

### User Story 3 - Repository Cleanup (Priority: P3)

A developer contributing to the project wants a clean repository without backup files or incomplete artifacts. The presence of `linux.c.backup` suggests an incomplete migration and adds confusion.

**Why this priority**: Clean repository state improves maintainability and reduces confusion for contributors. Lower priority because it doesn't affect functionality.

**Independent Test**: Can be tested by checking that no `.backup` files exist in the source tree and that git status shows a clean state.

**Acceptance Scenarios**:

1. **Given** the repository, **When** searching for backup files, **Then** no `.backup` files exist in the `src/` directory.
2. **Given** the current git branch, **When** all cleanup is complete, **Then** git status shows no untracked backup files.

---

### Edge Cases

- What happens when `stats()` is called while profiling is not active? Returns `None` (existing behavior preserved).
- How does macOS handle SIGPROF blocking during timer destroy when the signal was never installed? Should be a no-op without errors.
- What happens if dropped samples counter overflows? Uses 64-bit atomic counters; overflow is practically impossible within profiling session lifetimes.

## Requirements *(mandatory)*

### Functional Requirements

#### Statistics API Completion

- **FR-001**: System MUST return the actual dropped samples count from the native ring buffer via `stats()` and `stop()` APIs.
- **FR-002**: System MUST calculate and return an overhead estimate percentage based on profiling metrics.
- **FR-003**: Python API MUST propagate native extension statistics accurately to user-facing data classes.

#### macOS Signal Safety

- **FR-004**: macOS `platform_timer_destroy()` MUST block SIGPROF before cleanup to prevent race conditions.
- **FR-005**: macOS `platform_timer_destroy()` MUST drain pending signals using `sigtimedwait()` instead of relying on `nanosleep()`.
- **FR-006**: macOS `platform_timer_destroy()` MUST restore original signal mask after cleanup.

#### Repository Cleanup

- **FR-007**: Repository MUST NOT contain backup files (`.backup` suffix) in the source tree.
- **FR-008**: Git repository MUST have a clean state with no untracked generated or backup files.

### Key Entities

- **ProfilerStats**: Extended to include accurate `dropped_samples` from native layer and calculated `overhead_estimate_pct`.
- **Profile**: Extended to include accurate `dropped_count` from native layer.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: `spprof.stats().dropped_samples` returns non-zero when ring buffer has experienced drops during high-frequency sampling.
- **SC-002**: `spprof.stats().overhead_estimate_pct` returns a reasonable positive value when profiling is active.
- **SC-003**: macOS profiler can complete 1000 start/stop cycles without crashes or hangs.
- **SC-004**: No `.backup` files exist in the `src/` directory after cleanup.
- **SC-005**: Existing test suite passes on all platforms after changes.

## Assumptions

- The native C extension already tracks dropped samples internally; the Python layer just needs to expose this value.
- macOS supports the same signal blocking and `sigtimedwait()` API as Linux (POSIX-compliant).
- The `linux.c.backup` file is no longer needed as the original state is preserved in git history.
- Overhead estimation can be approximated based on sample count, interval, and elapsed time without requiring precise CPU cycle counting.
