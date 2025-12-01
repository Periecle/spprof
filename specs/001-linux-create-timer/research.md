# Research: Linux timer_create Robustness Improvements

**Feature Branch**: `001-linux-create-timer`  
**Date**: 2025-12-01  
**Purpose**: Resolve technical unknowns and document architectural decisions for improving the Linux timer implementation.

---

## 1. Dynamic Thread Registry Design

### Decision: Hash Table with TID Key, Dynamically Allocated

Replace the fixed `ThreadTimer g_thread_timers[MAX_TRACKED_THREADS]` array with a dynamically growing hash table keyed by thread ID (TID).

### Current Implementation Analysis

```c
// Current: Fixed-size array with linear search
#define MAX_TRACKED_THREADS 256

static ThreadTimer g_thread_timers[MAX_TRACKED_THREADS];
static int g_thread_count = 0;

// O(n) lookup for unregistration
for (int i = 0; i < g_thread_count; i++) {
    if (g_thread_timers[i].tid == tid) {
        g_thread_timers[i].active = 0;
        break;
    }
}
```

**Problems**:
- Hard limit of 256 threads
- O(n) lookup for thread operations
- Memory wasted when few threads active
- No cleanup of inactive entries

### Proposed Implementation: `uthash` Header-Only Library

```c
#include "uthash.h"

typedef struct {
    pid_t tid;              // Key: thread ID
    timer_t timer_id;       // POSIX timer handle
    uint64_t overruns;      // Accumulated timer overruns
    int active;             // Is timer currently running?
    UT_hash_handle hh;      // Makes this structure hashable
} ThreadTimerEntry;

static ThreadTimerEntry* g_thread_registry = NULL;  // Hash table head
static pthread_rwlock_t g_registry_lock = PTHREAD_RWLOCK_INITIALIZER;
```

### Thread-Safe Access Pattern

```c
// Registration (write lock)
int registry_add_thread(pid_t tid, timer_t timer_id) {
    ThreadTimerEntry* entry = malloc(sizeof(ThreadTimerEntry));
    if (!entry) return -1;
    
    entry->tid = tid;
    entry->timer_id = timer_id;
    entry->overruns = 0;
    entry->active = 1;
    
    pthread_rwlock_wrlock(&g_registry_lock);
    HASH_ADD_INT(g_thread_registry, tid, entry);
    pthread_rwlock_unlock(&g_registry_lock);
    
    return 0;
}

// Lookup (read lock) - for statistics
ThreadTimerEntry* registry_find_thread(pid_t tid) {
    ThreadTimerEntry* entry = NULL;
    pthread_rwlock_rdlock(&g_registry_lock);
    HASH_FIND_INT(g_thread_registry, &tid, entry);
    pthread_rwlock_unlock(&g_registry_lock);
    return entry;
}

// Unregistration (write lock)
int registry_remove_thread(pid_t tid) {
    ThreadTimerEntry* entry = NULL;
    
    pthread_rwlock_wrlock(&g_registry_lock);
    HASH_FIND_INT(g_thread_registry, &tid, entry);
    if (entry) {
        HASH_DEL(g_thread_registry, entry);
    }
    pthread_rwlock_unlock(&g_registry_lock);
    
    if (entry) {
        if (entry->timer_id) {
            timer_delete(entry->timer_id);
        }
        free(entry);
        return 0;
    }
    return -1;  // Not found
}
```

### Rationale
- `uthash` is header-only, BSD-licensed, battle-tested
- O(1) average case for add/find/delete operations
- Dynamic growth - no artificial limits
- Memory only allocated for active threads
- RWLock allows concurrent stat reads without blocking timer operations

### Alternatives Considered

| Alternative | Rejected Because |
|-------------|------------------|
| Larger fixed array (4096) | Still arbitrary limit; wastes memory |
| `pthread_key_t` TLS | Doesn't support enumeration for cleanup |
| Linux `khash` | Less documented, similar complexity |
| Custom hash table | `uthash` is well-tested and maintained |

---

## 2. Timer Overrun Handling

### Decision: Per-Thread Overrun Tracking with Aggregation

Use `timer_getoverrun()` to track missed timer expirations and aggregate per thread.

### Linux timer_getoverrun() Behavior

```c
// After signal delivery, get count of missed expirations
int overrun = timer_getoverrun(timer_id);
// Returns: number of additional timer expirations since last signal delivery
// Returns: -1 on error (errno set)
```

**Key insight**: Overruns are only available immediately after timer signal delivery. Must capture in signal handler context.

### Implementation Strategy

```c
// In signal handler (async-signal-safe)
void spprof_signal_handler(int signum, siginfo_t* info, void* ucontext) {
    // ... existing handler code ...
    
    // Capture overrun count before processing
    // Note: We can't update registry in signal handler (not async-signal-safe)
    // Instead, include overrun in the sample metadata
    
    timer_t timer_id = info->si_value.sival_ptr;  // If we store timer_id in sigval
    int overrun = timer_getoverrun(timer_id);
    
    // Store overrun with sample for later aggregation
    sample.timer_overrun = (overrun > 0) ? overrun : 0;
}

// In consumer thread (safe context) - aggregate overruns
void process_sample(RawSample* sample) {
    if (sample->timer_overrun > 0) {
        atomic_fetch_add(&g_total_overruns, sample->timer_overrun);
    }
}
```

### Alternative: Post-Signal Aggregation

Since we can't safely update the registry from signal handler, aggregate overruns globally:

```c
// Global atomic counter (signal-safe reads)
static _Atomic uint64_t g_total_overruns = 0;

// After timer_delete, check final overrun count
void cleanup_thread_timer(timer_t timer_id) {
    int final_overrun = timer_getoverrun(timer_id);
    if (final_overrun > 0) {
        atomic_fetch_add(&g_total_overruns, final_overrun);
    }
    timer_delete(timer_id);
}

// Statistics API
uint64_t platform_get_timer_overruns(void) {
    return atomic_load(&g_total_overruns);
}
```

### Rationale
- `timer_getoverrun()` is the only way to detect missed expirations
- Global atomic counter avoids lock contention
- Statistics are eventually consistent (good enough for profiling)

### Alternatives Considered

| Alternative | Rejected Because |
|-------------|------------------|
| Ignore overruns | Users can't assess profiling accuracy |
| Per-thread atomic counters | More memory, not accessible from signal handler |
| Compensate by injecting samples | Would distort timing; complex |

---

## 3. Race-Free Timer Cleanup

### Decision: Signal Blocking + Ordered Cleanup

Block signals during timer deletion to prevent race between `timer_delete()` and signal delivery.

### Race Condition Analysis

**Problem**: Current implementation has a race:

```c
// RACE: Signal may be in-flight when timer is deleted
timer_delete(timer_id);  // Timer deleted
// ... but signal was already queued/in-transit ...
// Signal handler runs with invalid timer_id
```

**Current Workaround**:
```c
// Hacky 1ms sleep - unreliable
struct timespec ts = {0, 1000000};
nanosleep(&ts, NULL);
```

### Robust Solution: Signal Blocking

```c
int platform_timer_destroy(void) {
    sigset_t block_set, old_set;
    
    // Block SIGPROF to prevent signal delivery during cleanup
    sigemptyset(&block_set);
    sigaddset(&block_set, SPPROF_SIGNAL);
    pthread_sigmask(SIG_BLOCK, &block_set, &old_set);
    
    // Stop accepting samples in handler
    signal_handler_stop();
    
    // Delete main thread timer
    if (g_main_timer != NULL) {
        timer_delete(g_main_timer);
        g_main_timer = NULL;
    }
    
    // Delete thread-local timer
    if (tl_timer_active && tl_timer_id != NULL) {
        timer_delete(tl_timer_id);
        tl_timer_id = NULL;
        tl_timer_active = 0;
    }
    
    // Drain any pending signals (they'll be discarded)
    // sigtimedwait with zero timeout consumes pending signals
    struct timespec timeout = {0, 0};
    siginfo_t info;
    while (sigtimedwait(&block_set, &info, &timeout) > 0) {
        // Discard pending SIGPROF
    }
    
    // Restore original signal mask
    pthread_sigmask(SIG_SETMASK, &old_set, NULL);
    
    // Now safe to uninstall signal handler
    signal_handler_uninstall(SPPROF_SIGNAL);
    
    return 0;
}
```

### Registry Cleanup During Shutdown

```c
void registry_cleanup_all(void) {
    sigset_t block_set, old_set;
    sigemptyset(&block_set);
    sigaddset(&block_set, SPPROF_SIGNAL);
    
    // Block signals during full cleanup
    pthread_sigmask(SIG_BLOCK, &block_set, &old_set);
    
    pthread_rwlock_wrlock(&g_registry_lock);
    
    ThreadTimerEntry *entry, *tmp;
    HASH_ITER(hh, g_thread_registry, entry, tmp) {
        if (entry->timer_id) {
            timer_delete(entry->timer_id);
        }
        HASH_DEL(g_thread_registry, entry);
        free(entry);
    }
    g_thread_registry = NULL;
    
    pthread_rwlock_unlock(&g_registry_lock);
    pthread_sigmask(SIG_SETMASK, &old_set, NULL);
}
```

### Rationale
- Signal blocking is the only reliable way to prevent races
- `sigtimedwait` drains pending signals without side effects
- Ordered cleanup: block → delete → drain → unblock → uninstall

### Alternatives Considered

| Alternative | Rejected Because |
|-------------|------------------|
| Longer sleep | Still a race; wastes time |
| Reference counting timers | Complex; still doesn't prevent in-flight signals |
| Ignore the race | Can crash on invalid memory access |

---

## 4. Pause/Resume Implementation

### Decision: Zero-Interval Timer Disarm/Rearm

Use `timer_settime()` with zero interval to pause, restore original interval to resume.

### Implementation

```c
static uint64_t g_saved_interval_ns = 0;
static int g_paused = 0;

int platform_timer_pause(void) {
    if (g_paused || !g_main_timer) {
        return 0;  // Already paused or no timer
    }
    
    // Save current interval
    struct itimerspec current;
    if (timer_gettime(g_main_timer, &current) < 0) {
        return -1;
    }
    g_saved_interval_ns = current.it_interval.tv_sec * 1000000000ULL 
                        + current.it_interval.tv_nsec;
    
    // Disarm timer (set interval to zero)
    struct itimerspec zero = {0};
    if (timer_settime(g_main_timer, 0, &zero, NULL) < 0) {
        return -1;
    }
    
    // Pause all registered thread timers
    pthread_rwlock_rdlock(&g_registry_lock);
    ThreadTimerEntry *entry, *tmp;
    HASH_ITER(hh, g_thread_registry, entry, tmp) {
        if (entry->active && entry->timer_id) {
            timer_settime(entry->timer_id, 0, &zero, NULL);
        }
    }
    pthread_rwlock_unlock(&g_registry_lock);
    
    signal_handler_stop();  // Stop accepting samples
    g_paused = 1;
    return 0;
}

int platform_timer_resume(void) {
    if (!g_paused || !g_main_timer) {
        return 0;  // Not paused or no timer
    }
    
    signal_handler_start();  // Resume accepting samples
    
    // Restore timer interval
    struct itimerspec its;
    its.it_value.tv_sec = g_saved_interval_ns / 1000000000ULL;
    its.it_value.tv_nsec = g_saved_interval_ns % 1000000000ULL;
    its.it_interval = its.it_value;
    
    if (timer_settime(g_main_timer, 0, &its, NULL) < 0) {
        return -1;
    }
    
    // Resume all registered thread timers
    pthread_rwlock_rdlock(&g_registry_lock);
    ThreadTimerEntry *entry, *tmp;
    HASH_ITER(hh, g_thread_registry, entry, tmp) {
        if (entry->active && entry->timer_id) {
            timer_settime(entry->timer_id, 0, &its, NULL);
        }
    }
    pthread_rwlock_unlock(&g_registry_lock);
    
    g_paused = 0;
    return 0;
}
```

### Rationale
- `timer_settime` with zero interval is documented to disarm the timer
- Timer handle remains valid - no need to recreate
- More efficient than delete/recreate cycle
- Allows preserving timer state (overrun counts, etc.)

### Alternatives Considered

| Alternative | Rejected Because |
|-------------|------------------|
| Delete and recreate timers | Loses state; higher overhead |
| Block signal during pause | Signals still generated (wastes CPU) |
| Handler flag only | Timer still fires; unnecessary overhead |

---

## 5. Thread-Local Storage Considerations

### Decision: Keep TLS for Fast Path, Registry for Management

Retain thread-local `timer_t` for fast access in hot path; use registry for enumeration and cleanup.

### Current TLS Pattern

```c
static __thread timer_t tl_timer_id = NULL;
static __thread int tl_timer_active = 0;
```

**Benefit**: No lock needed for per-thread timer access in normal operations.

### Integration with Registry

```c
int platform_register_thread(uint64_t interval_ns) {
    if (tl_timer_active) {
        return 0;  // Fast path: already registered
    }
    
    pid_t tid = syscall(SYS_gettid);
    timer_t timer_id;
    
    // Create timer (existing code)
    struct sigevent sev = {0};
    sev.sigev_notify = SIGEV_THREAD_ID;
    sev.sigev_signo = SPPROF_SIGNAL;
    sev._sigev_un._tid = tid;
    
    if (timer_create(CLOCK_THREAD_CPUTIME_ID, &sev, &timer_id) < 0) {
        return -1;
    }
    
    // ... configure timer ...
    
    // Update TLS (fast path)
    tl_timer_id = timer_id;
    tl_timer_active = 1;
    
    // Update registry (management path)
    registry_add_thread(tid, timer_id);
    
    return 0;
}
```

### Rationale
- TLS provides lock-free access for common operations
- Registry enables enumeration for pause/resume/cleanup
- Dual storage is acceptable overhead for robustness

---

## 6. Error Handling and Resource Limits

### Decision: Graceful Degradation with Logging

Handle `timer_create` failures due to system limits gracefully without crashing.

### System Limits

```c
// Check relevant limits
#include <sys/resource.h>

void check_timer_limits(void) {
    struct rlimit rlim;
    
    // RLIMIT_SIGPENDING limits queued signals (related to timers)
    if (getrlimit(RLIMIT_SIGPENDING, &rlim) == 0) {
        // Soft limit: rlim.rlim_cur
        // Hard limit: rlim.rlim_max
    }
    
    // /proc/sys/kernel/pid_max affects timer limit indirectly
    // Typical limit: SIGPENDING default is ~128k on modern systems
}
```

### Error Handling Pattern

```c
int platform_register_thread(uint64_t interval_ns) {
    // ... create timer ...
    
    if (timer_create(CLOCK_THREAD_CPUTIME_ID, &sev, &timer_id) < 0) {
        int saved_errno = errno;
        
        switch (saved_errno) {
            case EAGAIN:
                // Resource temporarily unavailable - retry once
                usleep(1000);
                if (timer_create(CLOCK_THREAD_CPUTIME_ID, &sev, &timer_id) < 0) {
                    // Log warning, continue without this thread
                    atomic_fetch_add(&g_timer_create_failures, 1);
                    return -1;
                }
                break;
                
            case ENOMEM:
                // Out of memory - log and fail gracefully
                atomic_fetch_add(&g_timer_create_failures, 1);
                return -1;
                
            case EINVAL:
                // Invalid arguments - programming error
                return -1;
                
            default:
                atomic_fetch_add(&g_timer_create_failures, 1);
                return -1;
        }
    }
    
    // ... continue with registration ...
    return 0;
}

// Statistics API
uint64_t platform_get_timer_failures(void) {
    return atomic_load(&g_timer_create_failures);
}
```

### Rationale
- Single retry for transient failures (EAGAIN)
- Failure counter exposed in statistics
- Thread continues without profiling rather than crashing
- Existing threads continue to be profiled

---

## Summary of Key Decisions

| Area | Decision | Key Constraint |
|------|----------|----------------|
| Thread Registry | uthash hash table | O(1) lookup, dynamic growth |
| Thread Safety | RWLock for registry | Concurrent reads during profiling |
| Overrun Tracking | Global atomic counter | Can't update registry from signal handler |
| Cleanup | Signal blocking + drain | Must prevent race with in-flight signals |
| Pause/Resume | Zero-interval disarm | Preserves timer handles and state |
| Error Handling | Graceful degradation | Never crash on resource limits |

