# Internal C API Contract: Thread Timer Registry

**Feature Branch**: `001-linux-create-timer`  
**Date**: 2025-12-01  
**Scope**: Linux platform implementation (`src/spprof/_ext/platform/linux.c`)

---

## Overview

This document defines the internal C API for the thread timer registry, which replaces the fixed-size array with a dynamic hash table for thread management.

**Note**: These are internal APIs, not exposed to Python. The public platform API (`platform.h`) remains unchanged.

---

## 1. Registry Management Functions

### `registry_init`

Initialize the thread registry.

```c
/**
 * Initialize the thread timer registry.
 *
 * Must be called once during platform_init().
 * NOT thread-safe - call only from main thread during startup.
 *
 * @return 0 on success, -1 on error
 */
int registry_init(void);
```

**Behavior**:
- Initializes RWLock
- Sets registry head to NULL
- Idempotent (safe to call multiple times)

**Errors**: None (initialization always succeeds)

---

### `registry_cleanup`

Destroy the thread registry and free all entries.

```c
/**
 * Clean up the thread timer registry.
 *
 * Deletes all timers and frees all memory.
 * Must block SIGPROF before calling.
 * NOT thread-safe - call only during shutdown.
 */
void registry_cleanup(void);
```

**Behavior**:
- Acquires write lock
- Iterates all entries, calling `timer_delete()` for each
- Frees all entry memory
- Sets registry head to NULL
- Releases lock

**Preconditions**:
- SIGPROF must be blocked (caller responsibility)
- No other threads should be registering/unregistering

---

## 2. Thread Entry Functions

### `registry_add_thread`

Add a new thread to the registry.

```c
/**
 * Add a thread timer entry to the registry.
 *
 * Thread-safe (acquires write lock).
 *
 * @param tid Thread ID (from gettid())
 * @param timer_id POSIX timer handle from timer_create()
 * @return 0 on success, -1 on error (ENOMEM or duplicate TID)
 */
int registry_add_thread(pid_t tid, timer_t timer_id);
```

**Behavior**:
- Allocates new `ThreadTimerEntry`
- Initializes fields: `active=1`, `overruns=0`
- Acquires write lock
- Adds to hash table (HASH_ADD_INT)
- Releases lock

**Errors**:
| Error | Condition | Errno |
|-------|-----------|-------|
| -1 | `malloc()` failed | `ENOMEM` |
| -1 | TID already exists | `EEXIST` |

**Thread Safety**: Full (uses write lock)

---

### `registry_find_thread`

Look up a thread entry by TID.

```c
/**
 * Find a thread timer entry by TID.
 *
 * Thread-safe (acquires read lock).
 * Returns pointer to entry - do NOT free or modify without lock.
 *
 * @param tid Thread ID to look up
 * @return Pointer to entry, or NULL if not found
 */
ThreadTimerEntry* registry_find_thread(pid_t tid);
```

**Behavior**:
- Acquires read lock
- Performs hash lookup (HASH_FIND_INT)
- Releases lock
- Returns pointer (may be stale if entry is removed)

**Thread Safety**: Full (uses read lock)

**Warning**: Returned pointer validity is not guaranteed after function returns. For long-lived access, copy needed fields while holding lock.

---

### `registry_remove_thread`

Remove a thread from the registry.

```c
/**
 * Remove a thread timer entry from the registry.
 *
 * Also deletes the associated POSIX timer.
 * Thread-safe (acquires write lock).
 *
 * @param tid Thread ID to remove
 * @return 0 on success, -1 if not found
 */
int registry_remove_thread(pid_t tid);
```

**Behavior**:
- Acquires write lock
- Looks up entry by TID
- If found: removes from hash, releases lock, deletes timer, frees memory
- If not found: releases lock, returns -1

**Errors**:
| Error | Condition |
|-------|-----------|
| -1 | TID not found in registry |

**Thread Safety**: Full (uses write lock)

---

## 3. Iteration Functions

### `registry_count`

Get number of registered threads.

```c
/**
 * Get the number of registered thread timers.
 *
 * Thread-safe (acquires read lock).
 *
 * @return Number of entries in registry
 */
size_t registry_count(void);
```

**Behavior**:
- Acquires read lock
- Returns `HASH_COUNT(g_thread_registry)`
- Releases lock

**Thread Safety**: Full (uses read lock)

---

### `registry_for_each`

Iterate over all entries with callback.

```c
/**
 * Iterate over all registry entries.
 *
 * Callback is invoked with read lock held - do NOT call registry functions
 * from within callback (will deadlock).
 *
 * @param callback Function to call for each entry
 * @param userdata Opaque pointer passed to callback
 */
typedef void (*registry_callback_t)(ThreadTimerEntry* entry, void* userdata);

void registry_for_each(registry_callback_t callback, void* userdata);
```

**Behavior**:
- Acquires read lock
- Calls `callback(entry, userdata)` for each entry
- Releases lock

**Thread Safety**: Full (callback runs under read lock)

**Warning**: Callback must not:
- Call any `registry_*` functions (deadlock)
- Block for extended periods (starves writers)
- Modify entry fields without additional synchronization

---

## 4. Timer Operations

### `registry_pause_all`

Pause all thread timers.

```c
/**
 * Pause all registered thread timers.
 *
 * Sets timer interval to zero (disarms) without deleting.
 * Thread-safe (acquires read lock).
 *
 * @return 0 on success, -1 on error
 */
int registry_pause_all(void);
```

**Behavior**:
- Acquires read lock
- For each entry where `active == 1`:
  - Calls `timer_settime()` with zero interval
  - Sets `entry->active = 0`
- Releases lock

**Thread Safety**: Full (uses read lock, atomic active flag updates)

---

### `registry_resume_all`

Resume all paused thread timers.

```c
/**
 * Resume all paused thread timers.
 *
 * Restores saved interval to all paused timers.
 * Thread-safe (acquires read lock).
 *
 * @param interval_ns Interval to set (nanoseconds)
 * @return 0 on success, -1 on error
 */
int registry_resume_all(uint64_t interval_ns);
```

**Behavior**:
- Acquires read lock
- For each entry where `active == 0` and `timer_id != NULL`:
  - Calls `timer_settime()` with specified interval
  - Sets `entry->active = 1`
- Releases lock

**Thread Safety**: Full (uses read lock, atomic active flag updates)

---

## 5. Statistics Functions

### `registry_get_total_overruns`

Get aggregated timer overruns.

```c
/**
 * Get total timer overruns across all threads.
 *
 * @return Sum of all timer overruns
 */
uint64_t registry_get_total_overruns(void);
```

**Behavior**:
- Returns `atomic_load(&g_total_overruns)`

**Thread Safety**: Full (atomic read)

---

### `registry_add_overruns`

Add to overrun counter (called from consumer thread).

```c
/**
 * Add to the global overrun counter.
 *
 * Called by sample consumer when processing samples with overrun data.
 * Thread-safe (atomic operation).
 *
 * @param count Number of overruns to add
 */
void registry_add_overruns(uint64_t count);
```

**Behavior**:
- `atomic_fetch_add(&g_total_overruns, count)`

**Thread Safety**: Full (atomic operation)

---

## 6. Error Handling

### Error Codes

All functions that can fail return `-1` on error with `errno` set:

| Function | Possible Errors |
|----------|-----------------|
| `registry_add_thread` | `ENOMEM`, `EEXIST` |
| `registry_remove_thread` | `ENOENT` |
| `registry_pause_all` | `EINVAL` (bad timer) |
| `registry_resume_all` | `EINVAL` (bad timer) |

### Failure Statistics

```c
/**
 * Get count of timer creation failures.
 *
 * Incremented when timer_create() fails during thread registration.
 *
 * @return Number of failed timer_create() calls
 */
uint64_t registry_get_create_failures(void);
```

---

## 7. Thread Safety Summary

| Function | Lock Type | Safe from Signal? |
|----------|-----------|-------------------|
| `registry_init` | None | No |
| `registry_cleanup` | Write | No |
| `registry_add_thread` | Write | No |
| `registry_find_thread` | Read | No |
| `registry_remove_thread` | Write | No |
| `registry_count` | Read | No |
| `registry_for_each` | Read | No |
| `registry_pause_all` | Read | No |
| `registry_resume_all` | Read | No |
| `registry_get_total_overruns` | None (atomic) | Yes |
| `registry_add_overruns` | None (atomic) | Yes |

**Note**: None of these functions should be called from the signal handler. The signal handler only reads atomic counters.

---

## 8. Public Platform API Changes

The following additions to `platform.h` are required:

```c
/* New functions */
int platform_timer_pause(void);
int platform_timer_resume(void);

/* Extended statistics (Linux-specific) */
void platform_get_extended_stats(
    uint64_t* samples_captured,
    uint64_t* samples_dropped,
    uint64_t* timer_overruns,
    uint64_t* timer_create_failures,
    uint64_t* registered_threads
);
```

**Backward Compatibility**: Existing `platform.h` functions remain unchanged:
- `platform_init()`
- `platform_cleanup()`
- `platform_timer_create()`
- `platform_timer_destroy()`
- `platform_register_thread()`
- `platform_unregister_thread()`
- `platform_get_stats()`



