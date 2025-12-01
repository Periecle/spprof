# Feature Specification: Internal API Only Frame Walking

**Feature Branch**: `004-internal-api-only`  
**Created**: 2025-12-01  
**Status**: Draft  
**Input**: User description: "Remove public API reliance for all version of python, make reliance on internal API and structures. It will bring more details in all versions and hopefully simplify code structure"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Consistent Profiling Details Across Python Versions (Priority: P1)

As a developer profiling a Python application, I want to receive the same level of detail (code objects, instruction pointers, frame ownership) regardless of which Python version I'm running, so that I can reliably analyze performance across different environments.

**Why this priority**: This is the core value proposition - eliminating the disparity between "internal API mode" (rich details) and "public API mode" (limited details). Currently, Python 3.9/3.10 users get degraded profiling quality.

**Independent Test**: Can be fully tested by profiling a sample application on Python 3.9, 3.10, 3.11, 3.12, 3.13, and 3.14, then comparing the captured frame data (code pointers, instruction pointers, frame ownership flags) - all should have equivalent detail levels.

**Acceptance Scenarios**:

1. **Given** a profiler running on Python 3.9, **When** capturing a call stack, **Then** the profiler captures code object pointers, instruction pointers, and frame ownership information (same fields as Python 3.14).
2. **Given** a profiler running on Python 3.10, **When** capturing a call stack, **Then** the captured frame data structure matches what is captured on Python 3.11+.
3. **Given** a profiler running on Python 3.11-3.14, **When** capturing a call stack, **Then** the profiler continues to capture full frame details as it does today with internal API mode.

---

### User Story 2 - Simplified Codebase Maintenance (Priority: P2)

As a maintainer of spprof, I want a single frame walking implementation that works across all supported Python versions, so that I can reduce code duplication, simplify testing, and make future Python version support easier.

**Why this priority**: Code simplification reduces maintenance burden and bug surface area. Currently, there are two separate code paths (internal API vs public API) that must be maintained and tested independently.

**Independent Test**: Can be verified by inspecting the codebase after implementation - there should be a single frame walking code path, with version-specific struct definitions but unified walking logic.

**Acceptance Scenarios**:

1. **Given** the updated codebase, **When** reviewing the frame walking implementation, **Then** there is one unified implementation path (not separate internal/public branches).
2. **Given** the updated codebase, **When** adding support for a new Python version, **Then** only the internal struct definitions need updating (not separate compat headers for public API).
3. **Given** the updated codebase, **When** the `SPPROF_USE_INTERNAL_API` flag is removed, **Then** all code paths use internal struct access by default.

---

### User Story 3 - Async-Signal-Safe Frame Walking on All Versions (Priority: P3)

As a developer profiling CPU-bound workloads, I want signal-based sampling to be fully async-signal-safe on all Python versions, so that profiling doesn't crash or corrupt data due to unsafe API calls in signal handlers.

**Why this priority**: Async-signal-safety is critical for production profiling reliability. The current public API fallback calls `Py_DECREF` in signal handlers, which is NOT async-signal-safe.

**Independent Test**: Can be tested by running stress tests with high sampling rates on Python 3.9/3.10 - the profiler should not crash or exhibit memory corruption.

**Acceptance Scenarios**:

1. **Given** a profiler running on Python 3.9 with signal-based sampling, **When** profiling under high load, **Then** no crashes occur due to unsafe API calls in signal handlers.
2. **Given** a profiler running on Python 3.10, **When** capturing frames in a signal handler, **Then** no Python C API calls that acquire locks or manage reference counts are made.
3. **Given** any supported Python version, **When** frame walking occurs, **Then** only direct struct field reads are performed (no function calls to Python runtime).

---

### Edge Cases

- What happens when profiling Python 3.9/3.10 code that uses generators or coroutines? The internal frame structures differ from regular frames.
- How does the system handle Python debug builds where struct layouts may include additional debug fields?
- What happens when profiling across Python minor version updates (e.g., 3.11.1 to 3.11.8) where internal struct layouts might have changed?
- How does the system behave if compiled against one Python version but run against another (ABI mismatch)?

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: System MUST define internal frame structures (`_spprof_InterpreterFrame`) for Python 3.9 and 3.10 that mirror CPython's actual internal layouts.
- **FR-002**: System MUST use direct struct field access for frame walking on ALL supported Python versions (3.9 through 3.14).
- **FR-003**: System MUST NOT call Python C API functions (`PyEval_GetFrame`, `PyFrame_GetBack`, `PyFrame_GetCode`, `Py_DECREF`) during frame capture.
- **FR-004**: System MUST capture instruction pointers on all Python versions to enable accurate line number resolution.
- **FR-005**: System MUST detect frame ownership type (thread, generator, frame object, C-stack) on all versions.
- **FR-006**: System MUST validate pointer safety before dereferencing internal structures.
- **FR-007**: System MUST provide version detection at compile time to select correct struct definitions.
- **FR-008**: System MUST remove or deprecate the `SPPROF_USE_INTERNAL_API` compile flag (internal API becomes the only mode).
- **FR-009**: System MUST remove the `compat/` header files that implement public API fallback (`py39.h`, `py311.h`, `py312.h`, `py313.h`, `py314.h`).
- **FR-010**: System MUST update the `framewalker.c` to remove the conditional compilation branches for public API mode.

### Key Entities

- **InterpreterFrame**: Internal Python frame structure containing code object, previous frame pointer, instruction pointer, and ownership flags. Layout varies by Python version.
- **ThreadState**: Python thread state containing pointer to current frame. Access method varies by version (cframe in 3.11-3.12, direct in 3.13+).
- **CFrame**: C-level frame linkage structure used in Python 3.11-3.12 to connect C and Python frames.
- **CodeObject**: Python code object containing function metadata. Accessed via frame's `f_code` or `f_executable` field depending on version.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: All supported Python versions (3.9-3.14) produce frame captures with identical data fields (code pointer, instruction pointer, frame ownership).
- **SC-002**: Frame capture operations complete without any Python C API calls (verifiable via code inspection - no `Py*` function calls in capture path).
- **SC-003**: Profiler stress tests pass on Python 3.9 and 3.10 at high sampling rates (1000+ samples/second) without crashes.
- **SC-004**: Codebase line count for frame walking implementation reduces by removing duplicate public API code paths.
- **SC-005**: Test coverage for frame walking remains at 100% across all supported Python versions.
- **SC-006**: No runtime performance regression - frame capture latency remains under current benchmarks.

## Assumptions

- Python's internal struct layouts for each minor version (3.9.x, 3.10.x, etc.) remain stable within that minor version series.
- The project will document which specific Python patch versions have been validated for struct layout compatibility.
- Python debug builds may require separate struct definitions if they include additional debug fields.
- The minimum supported Python version remains 3.9; older versions are not in scope.
- Free-threading builds (Python 3.13+ with `Py_GIL_DISABLED`) continue to require thread suspension for safe sampling, regardless of API mode.
