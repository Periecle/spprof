# API Contract: Darwin Mach Sampler Internal C API

**Feature**: 003-darwin-mach-sampler  
**Date**: 2024-12-01  
**Type**: Internal C API (not exposed to Python directly)

## Overview

This document defines the internal C API for the Darwin Mach-based sampler. These functions are called by `platform/darwin.c` to implement the platform abstraction layer.

## Module: darwin_mach.h

### Initialization

#### `mach_sampler_init`

Initialize the Mach sampler subsystem.

```c
/**
 * Initialize the Mach sampler.
 *
 * Must be called before any other mach_sampler_* functions.
 * Installs the pthread introspection hook for thread tracking.
 *
 * Thread safety: NOT thread-safe. Call once at module init.
 *
 * @return 0 on success, -1 on error (sets errno)
 *
 * Errors:
 *   ENOMEM - memory allocation failed
 *   EBUSY  - already initialized
 */
int mach_sampler_init(void);
```

#### `mach_sampler_cleanup`

Clean up the Mach sampler subsystem.

```c
/**
 * Clean up the Mach sampler.
 *
 * Stops sampling if running, removes hooks, frees resources.
 *
 * Thread safety: NOT thread-safe. Call once at module cleanup.
 */
void mach_sampler_cleanup(void);
```

---

### Sampling Control

#### `mach_sampler_start`

Start the sampler thread.

```c
/**
 * Start sampling.
 *
 * Creates and starts the sampler thread. Sampling begins immediately.
 *
 * @param interval_ns  Sampling interval in nanoseconds (1ms - 1s)
 * @param ringbuffer   Ring buffer to write samples to (must outlive sampler)
 *
 * @return 0 on success, -1 on error
 *
 * Errors:
 *   EINVAL - invalid interval or NULL ringbuffer
 *   EAGAIN - sampler already running
 *   ENOMEM - thread creation failed
 *
 * Preconditions:
 *   - mach_sampler_init() called
 *   - mach_sampler_start() not already called (or stopped)
 *
 * Postconditions:
 *   - Sampler thread running
 *   - Samples written to ringbuffer at interval_ns
 */
int mach_sampler_start(uint64_t interval_ns, RingBuffer* ringbuffer);
```

#### `mach_sampler_stop`

Stop the sampler thread.

```c
/**
 * Stop sampling.
 *
 * Signals the sampler thread to stop and waits for it to exit.
 * All suspended threads are guaranteed to be resumed before return.
 *
 * Thread safety: NOT thread-safe. Call from single control thread.
 *
 * @return 0 on success, -1 on error
 *
 * Errors:
 *   ESRCH - sampler not running
 *
 * Postconditions:
 *   - Sampler thread terminated
 *   - No threads left in suspended state
 *   - Statistics finalized
 */
int mach_sampler_stop(void);
```

---

### Configuration

#### `mach_sampler_set_native_unwinding`

Enable or disable native frame capture.

```c
/**
 * Enable or disable native (C-stack) frame capture.
 *
 * When enabled, captured frames include native return addresses.
 * Can be called while sampler is running.
 *
 * @param enabled  1 to enable, 0 to disable
 */
void mach_sampler_set_native_unwinding(int enabled);
```

#### `mach_sampler_get_native_unwinding`

Check if native unwinding is enabled.

```c
/**
 * Check if native unwinding is enabled.
 *
 * @return 1 if enabled, 0 if disabled
 */
int mach_sampler_get_native_unwinding(void);
```

---

### Statistics

#### `mach_sampler_get_stats`

Get sampling statistics.

```c
/**
 * Get sampling statistics.
 *
 * Thread safety: Safe to call from any thread while sampler is running.
 * Statistics are approximate (no locking).
 *
 * @param samples_captured  Output: samples successfully written to ringbuffer
 * @param samples_dropped   Output: samples dropped due to full buffer
 * @param threads_sampled   Output: total thread samples taken
 *
 * Any output parameter may be NULL to skip.
 */
void mach_sampler_get_stats(
    uint64_t* samples_captured,
    uint64_t* samples_dropped,
    uint64_t* threads_sampled
);
```

#### `mach_sampler_get_extended_stats`

Get detailed statistics.

```c
/**
 * Get extended statistics including timing information.
 *
 * @param samples_captured   Output: samples written
 * @param samples_dropped    Output: samples dropped
 * @param threads_sampled    Output: total thread samples
 * @param threads_skipped    Output: threads skipped (terminated, invalid)
 * @param total_suspend_ns   Output: cumulative thread suspension time
 * @param max_suspend_ns     Output: maximum single suspension time
 * @param walk_errors        Output: frame walking errors
 */
void mach_sampler_get_extended_stats(
    uint64_t* samples_captured,
    uint64_t* samples_dropped,
    uint64_t* threads_sampled,
    uint64_t* threads_skipped,
    uint64_t* total_suspend_ns,
    uint64_t* max_suspend_ns,
    uint64_t* walk_errors
);
```

---

### Thread Registry

#### `mach_sampler_thread_count`

Get number of tracked threads.

```c
/**
 * Get the number of threads currently tracked.
 *
 * Thread safety: Safe to call from any thread.
 *
 * @return Number of threads in registry
 */
size_t mach_sampler_thread_count(void);
```

---

## Module: darwin_mach_internal.h

Internal functions not exposed in public header.

### Thread Registry Operations

```c
/**
 * Initialize the thread registry.
 *
 * @param registry  Registry to initialize
 * @return 0 on success, -1 on error
 */
int registry_init(ThreadRegistry* registry);

/**
 * Clean up the thread registry.
 *
 * @param registry  Registry to clean up
 */
void registry_cleanup(ThreadRegistry* registry);

/**
 * Add a thread to the registry.
 *
 * Called from pthread introspection hook on THREAD_START.
 *
 * @param registry  Registry to add to
 * @param pthread   pthread handle of new thread
 */
void registry_add(ThreadRegistry* registry, pthread_t pthread);

/**
 * Mark a thread as terminated in the registry.
 *
 * Called from pthread introspection hook on THREAD_TERMINATE.
 *
 * @param registry  Registry to update
 * @param pthread   pthread handle of terminating thread
 */
void registry_remove(ThreadRegistry* registry, pthread_t pthread);

/**
 * Get a snapshot of threads for sampling.
 *
 * Copies current thread entries to output array.
 *
 * @param registry  Registry to snapshot
 * @param snapshot  Output snapshot structure
 */
void registry_snapshot(ThreadRegistry* registry, ThreadSnapshot* snapshot);

/**
 * Compact registry by removing invalid entries.
 *
 * Should be called periodically to reclaim space.
 *
 * @param registry  Registry to compact
 */
void registry_compact(ThreadRegistry* registry);
```

### Stack Walking

```c
/**
 * Walk the stack of a suspended thread.
 *
 * @param thread_entry  Thread to walk (must be suspended)
 * @param regs          Register state from thread_get_state
 * @param stack         Output captured stack
 * @param max_depth     Maximum frames to capture
 *
 * @return Number of frames captured, or -1 on error
 */
int walk_stack(
    const ThreadEntry* thread_entry,
    const RegisterState* regs,
    CapturedStack* stack,
    int max_depth
);

/**
 * Validate a frame pointer.
 *
 * @param fp           Frame pointer to validate
 * @param stack_base   High address of stack
 * @param stack_limit  Low address of stack
 *
 * @return 1 if valid, 0 if invalid
 */
int validate_frame_pointer(uintptr_t fp, uintptr_t stack_base, uintptr_t stack_limit);
```

### Timing

```c
/**
 * Convert nanoseconds to mach_absolute_time units.
 *
 * @param ns  Nanoseconds
 * @return Mach absolute time units
 */
uint64_t ns_to_mach(uint64_t ns);

/**
 * Convert mach_absolute_time units to nanoseconds.
 *
 * @param mach  Mach absolute time units
 * @return Nanoseconds
 */
uint64_t mach_to_ns(uint64_t mach);
```

---

## Error Handling

All functions follow these conventions:

| Return Type | Success | Failure |
|------------|---------|---------|
| `int` | 0 | -1, errno set |
| Pointer | Non-NULL | NULL, errno set |
| `void` | N/A | N/A |

### Error Codes

| errno | Meaning |
|-------|---------|
| `EINVAL` | Invalid argument |
| `ENOMEM` | Memory allocation failed |
| `EAGAIN` | Resource temporarily unavailable (e.g., already running) |
| `ESRCH` | No such process/thread |
| `EBUSY` | Resource busy |

---

## Thread Safety Summary

| Function | Thread Safety |
|----------|--------------|
| `mach_sampler_init` | NOT safe - call once |
| `mach_sampler_cleanup` | NOT safe - call once |
| `mach_sampler_start` | NOT safe - single control thread |
| `mach_sampler_stop` | NOT safe - single control thread |
| `mach_sampler_set_native_unwinding` | Safe - atomic |
| `mach_sampler_get_native_unwinding` | Safe - atomic |
| `mach_sampler_get_stats` | Safe - volatile reads |
| `mach_sampler_get_extended_stats` | Safe - volatile reads |
| `mach_sampler_thread_count` | Safe - locked read |
| `registry_*` (internal) | Protected by registry lock |
| `walk_stack` (internal) | Sampler thread only |

---

## Integration with Platform API

The `darwin.c` platform implementation maps these functions to the platform API:

| Platform API | Mach Sampler Function |
|-------------|----------------------|
| `platform_init` | `mach_sampler_init` |
| `platform_cleanup` | `mach_sampler_cleanup` |
| `platform_timer_create` | `mach_sampler_start` |
| `platform_timer_destroy` | `mach_sampler_stop` |
| `platform_get_stats` | `mach_sampler_get_stats` |
| `signal_handler_set_native` | `mach_sampler_set_native_unwinding` |
| `signal_handler_samples_captured` | via `mach_sampler_get_stats` |
| `signal_handler_samples_dropped` | via `mach_sampler_get_stats` |

