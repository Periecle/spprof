# Data Model: Darwin Mach-Based Sampler

**Feature**: 003-darwin-mach-sampler  
**Date**: 2024-12-01

## Overview

This document defines the data structures for the Darwin Mach-based sampler. The design reuses existing `RawSample` and `RingBuffer` structures where possible, adding only the minimum new structures required for Mach-specific functionality.

## Core Entities

### 1. ThreadRegistry

Maintains a synchronized collection of active thread Mach ports for sampling.

```c
/**
 * Thread entry in the registry
 */
typedef struct {
    thread_act_t mach_port;     // Mach thread port for suspend/resume/get_state
    pthread_t pthread;          // Original pthread handle
    uint64_t thread_id;         // Cached OS thread ID for RawSample
    uintptr_t stack_base;       // Stack high address (from pthread)
    uintptr_t stack_limit;      // Stack low address (from pthread)
    int is_valid;               // 0 if thread has terminated
} ThreadEntry;

/**
 * Thread registry - tracks all active threads in the process
 */
typedef struct {
    pthread_mutex_t lock;           // Protects all fields
    ThreadEntry* entries;           // Dynamic array of thread entries
    size_t count;                   // Number of valid entries
    size_t capacity;                // Allocated capacity
    thread_act_t sampler_thread;    // Mach port of sampler (excluded from sampling)
    int hook_installed;             // 1 if introspection hook active
    pthread_introspection_hook_t prev_hook;  // Previous hook to chain
} ThreadRegistry;
```

**Invariants**:
- `entries` array contains no duplicate `mach_port` values
- `sampler_thread` is never in `entries` array
- Lock must be held for all modifications
- `is_valid` may become 0 between snapshot and suspend (race allowed, handled gracefully)

**State Transitions**:
```
Thread Lifecycle:
  [not exists] --THREAD_START--> [in registry, is_valid=1]
  [in registry, is_valid=1] --THREAD_TERMINATE--> [in registry, is_valid=0]
  [in registry, is_valid=0] --cleanup--> [removed from registry]
```

---

### 2. MachSamplerState

Global state for the Mach-based sampler.

```c
/**
 * Sampler configuration
 */
typedef struct {
    uint64_t interval_ns;           // Sampling interval in nanoseconds
    int native_unwinding;           // 1 to capture native frames
    int max_stack_depth;            // Maximum frames to capture
} MachSamplerConfig;

/**
 * Sampler statistics
 */
typedef struct {
    volatile uint64_t samples_captured;     // Successful samples written
    volatile uint64_t samples_dropped;      // Samples dropped (buffer full)
    volatile uint64_t threads_sampled;      // Total thread samples taken
    volatile uint64_t threads_skipped;      // Threads skipped (terminated, invalid)
    volatile uint64_t suspend_time_ns;      // Total time threads were suspended
    volatile uint64_t max_suspend_ns;       // Maximum single suspension time
    volatile uint64_t walk_errors;          // Frame walking errors
} MachSamplerStats;

/**
 * Main sampler state
 */
typedef struct {
    // Configuration
    MachSamplerConfig config;
    
    // Timing
    mach_timebase_info_data_t timebase;
    uint64_t interval_mach;             // Interval in mach_absolute_time units
    
    // Thread management
    pthread_t sampler_pthread;          // Sampler thread handle
    thread_act_t sampler_mach_thread;   // Sampler's own Mach port
    volatile int running;               // 1 while sampler should run
    
    // Thread registry
    ThreadRegistry registry;
    
    // Output
    RingBuffer* ringbuffer;             // Existing ring buffer (shared)
    
    // Statistics
    MachSamplerStats stats;
} MachSamplerState;
```

**Lifecycle**:
```
Sampler States:
  [uninitialized] --mach_sampler_init()--> [initialized]
  [initialized] --mach_sampler_start()--> [running]
  [running] --mach_sampler_stop()--> [stopped]
  [stopped] --mach_sampler_cleanup()--> [uninitialized]
```

---

### 3. ThreadSnapshot

Temporary structure for iterating threads during a sample cycle.

```c
/**
 * Maximum threads to sample in one cycle
 */
#define MACH_MAX_THREADS_PER_SAMPLE 256

/**
 * Snapshot of threads to sample (copied from registry)
 */
typedef struct {
    ThreadEntry entries[MACH_MAX_THREADS_PER_SAMPLE];
    size_t count;
} ThreadSnapshot;
```

**Usage**: Copied from registry under lock, then iterated without lock.

---

### 4. CapturedFrame

Intermediate structure for frame data during stack walk.

```c
/**
 * Single captured frame
 */
typedef struct {
    uintptr_t return_addr;      // Return address (caller's IP)
    uintptr_t frame_ptr;        // Frame pointer value
} CapturedFrame;

/**
 * Captured stack from one thread
 */
typedef struct {
    CapturedFrame frames[SPPROF_MAX_STACK_DEPTH];
    int depth;
    uint64_t thread_id;
    uint64_t timestamp;
    int truncated;              // 1 if stack deeper than max
    int error;                  // Non-zero on walk error
} CapturedStack;
```

---

### 5. Architecture-Specific Register State

```c
/**
 * Unified register state (architecture-independent view)
 */
typedef struct {
    uintptr_t pc;       // Program counter (RIP on x86, PC on arm64)
    uintptr_t sp;       // Stack pointer (RSP on x86, SP on arm64)
    uintptr_t fp;       // Frame pointer (RBP on x86, FP on arm64)
    uintptr_t lr;       // Link register (arm64 only, 0 on x86)
} RegisterState;

/**
 * Extract unified register state from architecture-specific state
 */
#if defined(__x86_64__)
static inline void extract_registers(x86_thread_state64_t* state, RegisterState* out) {
    out->pc = state->__rip;
    out->sp = state->__rsp;
    out->fp = state->__rbp;
    out->lr = 0;  // x86 uses stack for return addresses
}
#elif defined(__arm64__)
static inline void extract_registers(arm_thread_state64_t* state, RegisterState* out) {
    out->pc = arm_thread_state64_get_pc(*state);
    out->sp = arm_thread_state64_get_sp(*state);
    out->fp = arm_thread_state64_get_fp(*state);
    out->lr = arm_thread_state64_get_lr(*state);
}
#endif
```

---

## Relationship to Existing Structures

### RawSample (unchanged)

The existing `RawSample` structure from `ringbuffer.h` is reused without modification:

```c
// From ringbuffer.h - DO NOT MODIFY
typedef struct {
    uint64_t timestamp;
    uint64_t thread_id;
    int depth;
    int _padding;
    uintptr_t frames[SPPROF_MAX_STACK_DEPTH];
    uintptr_t instr_ptrs[SPPROF_MAX_STACK_DEPTH];
} RawSample;
```

**Mapping from CapturedStack**:
- `timestamp` ← `CapturedStack.timestamp`
- `thread_id` ← `CapturedStack.thread_id`
- `depth` ← `CapturedStack.depth`
- `frames[i]` ← `CapturedStack.frames[i].return_addr` (native IPs)
- `instr_ptrs[i]` ← 0 (not used for native frames; Python resolution happens later)

### RingBuffer (unchanged)

The existing `RingBuffer` is reused as-is. The Mach sampler writes `RawSample` entries just like the signal handler does.

---

## Memory Layout

### Thread Registry Growth

```
Initial: capacity = 32
Growth: capacity *= 2 when count == capacity
Max: capacity = 4096 (reasonable limit; most apps have <100 threads)

Memory per entry: sizeof(ThreadEntry) = ~48 bytes
Max memory: 4096 * 48 = ~192KB
```

### Stack Walk Memory

```
Per walk: CapturedStack on sampler thread stack
Size: sizeof(CapturedStack) = ~2KB

No heap allocation during sampling loop.
```

---

## Thread Safety

| Structure | Access Pattern | Synchronization |
|-----------|---------------|-----------------|
| ThreadRegistry | Multi-producer (hook), single-consumer (sampler) | pthread_mutex |
| MachSamplerState | Single-writer (control), single-reader (sampler) | Atomic volatile |
| ThreadSnapshot | Single-thread (sampler only) | None needed |
| CapturedStack | Single-thread (sampler only) | None needed |
| RingBuffer | Single-producer (sampler), single-consumer (resolver) | Lock-free atomics |
| MachSamplerStats | Single-writer (sampler), multi-reader | Atomic volatile |

---

## Validation Rules

### ThreadEntry
- `mach_port != MACH_PORT_NULL`
- `stack_base > stack_limit`
- `stack_base - stack_limit >= 4096` (minimum 4KB stack)

### MachSamplerConfig
- `interval_ns >= 1000000` (min 1ms)
- `interval_ns <= 1000000000` (max 1s)
- `max_stack_depth > 0 && max_stack_depth <= SPPROF_MAX_STACK_DEPTH`

### CapturedFrame
- `frame_ptr` within thread's `[stack_limit, stack_base]`
- `frame_ptr` is 8-byte aligned
- `return_addr != 0` (except for bottom frame)



