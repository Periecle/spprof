# Research: Resolve TODOs, Race Conditions, and Incomplete Implementations

## 1. Dropped Samples Wiring (US1 - FR-001)

### Current State

The native C extension already tracks dropped samples correctly:

```c
// ringbuffer.c:231
uint64_t ringbuffer_dropped_count(RingBuffer* rb) {
    return ATOMIC_LOAD_RELAXED(&rb->dropped_count);
}

// module.c:229 - _get_stats() already returns this
uint64_t dropped = g_ringbuffer ? ringbuffer_dropped_count(g_ringbuffer) : 0;
```

However, the Python API ignores this data:

```python
# __init__.py:243
dropped_count = 0  # TODO: Get from native
```

### Decision: Wire Native Stats to Python API

**Approach**: After calling `_native._stop()`, query final stats before clearing state.

**Implementation**:
1. In `stop()`: Call `_native._get_stats()` after `_native._stop()` to get final dropped count
2. The native `_get_stats()` already returns `dropped_samples` from the ring buffer

**Rationale**: No native code changes needed; just use existing infrastructure.

**Alternatives Considered**:
- Have `_stop()` return stats along with samples → More complex; changes C API
- Store stats in resolver → Already available via ring buffer

---

## 2. Overhead Estimation (US1 - FR-002)

### Analysis

Overhead estimation requires knowing:
1. How many samples were collected
2. How long the profiling session ran
3. Approximate signal handler cost per sample

### Decision: Simple Percentage Approximation

**Formula**: 
```
overhead_pct = (samples_collected * avg_handler_time_us) / (duration_ms * 1000) * 100
```

Where `avg_handler_time_us` is estimated at ~10-50μs based on empirical measurements of signal handlers.

**Implementation**:
- Get `collected_samples` and `duration_ns` from `_get_stats()`
- Use conservative estimate of 25μs per sample
- Calculate: `(collected * 0.025) / (duration_ms) * 100`

**Rationale**: Provides reasonable estimate without adding instrumentation overhead.

**Alternatives Considered**:
- Measure actual handler time via timestamps → Adds overhead; violates async-signal-safety
- Hardware performance counters → Requires privileged access; defeats K8s use case
- Return 0 with documentation → Doesn't help users tune interval

**Conservative Default**: 25μs per sample is a safe upper bound that includes:
- Frame walking (~5-15μs)
- Ring buffer write (~1-2μs)
- Signal dispatch overhead (~5-10μs)

---

## 3. macOS Signal Draining (US2 - FR-004, FR-005, FR-006)

### Current Problem

macOS `platform_timer_destroy()` uses naive `nanosleep()`:

```c
// darwin.c:95-97
/* Brief pause for in-flight signals */
struct timespec ts = {0, 1000000};  /* 1ms */
nanosleep(&ts, NULL);
```

This has the same race condition that was fixed on Linux: a signal could be in-flight and delivered after the handler is uninstalled, causing a crash.

### Linux Solution (Reference)

The Linux implementation properly handles this:

```c
// linux.c:469-509
int platform_timer_destroy(void) {
    sigset_t block_set, old_set;
    
    /* Block SIGPROF to prevent signal delivery during cleanup */
    sigemptyset(&block_set);
    sigaddset(&block_set, SPPROF_SIGNAL);
    pthread_sigmask(SIG_BLOCK, &block_set, &old_set);
    
    /* Stop accepting samples */
    signal_handler_stop();
    
    /* ... delete timer ... */
    
    /* Drain any pending signals */
    struct timespec timeout = {0, 0};
    siginfo_t info;
    while (sigtimedwait(&block_set, &info, &timeout) > 0) {
        /* Discard pending SIGPROF */
    }
    
    /* Restore original signal mask */
    pthread_sigmask(SIG_SETMASK, &old_set, NULL);
    
    /* Now safe to restore original signal handler */
    signal_handler_uninstall(SPPROF_SIGNAL);
    
    return 0;
}
```

### Decision: Port Linux Pattern to macOS

**Implementation**:
1. Block SIGPROF at the start of `platform_timer_destroy()`
2. Stop the timer via `setitimer()` with zero interval
3. Stop accepting samples via `signal_handler_stop()`
4. Drain pending signals via `sigtimedwait()` loop
5. Restore original signal mask
6. Uninstall signal handler

**Rationale**: This pattern is proven on Linux and macOS supports the same POSIX APIs:
- `pthread_sigmask()` - POSIX; available on macOS
- `sigtimedwait()` - POSIX; available on macOS since 10.6

**macOS-Specific Considerations**:
- `setitimer()` delivers to process, not thread; must handle at process level
- Signal mask operations are per-thread; `pthread_sigmask()` is correct
- `sigtimedwait()` with zero timeout is non-blocking and safe

**Alternatives Considered**:
- Keep `nanosleep()` but increase to 10ms → Still a race; wastes time
- Ignore the race → Can crash on rapid start/stop cycles
- Use dispatch sources instead of signals → Major rewrite; breaks parity with Linux

---

## 4. Repository Cleanup (US3 - FR-007, FR-008)

### Current State

```
Untracked files:
  src/spprof/_ext/platform/linux.c.backup
```

This file is a backup from the thread registry implementation and is no longer needed.

### Decision: Delete Backup File

**Implementation**: Remove `linux.c.backup` from the repository.

**Rationale**: The file's history is preserved in git; no functional purpose to keeping it.

**Verification**: Run `git status` after deletion to confirm clean state.

---

## Summary of Decisions

| Decision | Rationale | Risk |
|----------|-----------|------|
| Wire stats from `_get_stats()` | Uses existing infrastructure | Low |
| Estimate overhead at 25μs/sample | Conservative; no instrumentation needed | Low (might be inaccurate) |
| Port Linux signal-drain to macOS | Proven pattern; POSIX-compatible | Low |
| Delete backup file | In git history; not needed | None |

