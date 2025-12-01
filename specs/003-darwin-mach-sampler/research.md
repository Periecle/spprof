# Research: Darwin Mach-Based Sampler

**Feature**: 003-darwin-mach-sampler  
**Date**: 2024-12-01  
**Status**: Complete

## Research Questions

### 1. How to discover threads on macOS without /proc?

**Decision**: Use `pthread_introspection_hook_install()` to track thread lifecycle.

**Rationale**: 
- Official Apple API (declared in `<pthread/introspection.h>`)
- Fires callbacks on pthread creation, start, terminate, and destroy
- Captures ALL pthreads including GCD dispatch worker threads
- Zero per-sample overhead (maintain pre-built list)

**Alternatives Considered**:

| Alternative | Rejected Because |
|-------------|-----------------|
| `task_threads()` | Per-sample syscall overhead; returns stale data; requires cleanup |
| Manual registration | Misses threads not under our control (GCD, libraries) |
| `/proc` style | Doesn't exist on macOS |

**API Signature**:
```c
#include <pthread/introspection.h>

typedef void (*pthread_introspection_hook_t)(
    unsigned int event,
    pthread_t thread, 
    void *addr, 
    size_t size
);

pthread_introspection_hook_t pthread_introspection_hook_install(
    pthread_introspection_hook_t hook
);

// Events:
// PTHREAD_INTROSPECTION_THREAD_CREATE  - thread allocated
// PTHREAD_INTROSPECTION_THREAD_START   - thread started running
// PTHREAD_INTROSPECTION_THREAD_TERMINATE - thread terminating
// PTHREAD_INTROSPECTION_THREAD_DESTROY - thread deallocated
```

**Implementation Notes**:
- Must chain to previous hook if non-NULL
- Use `THREAD_START` for adding (not CREATE - thread may not be fully initialized)
- Use `THREAD_TERMINATE` for removing (not DESTROY - thread_act_t may be invalid)
- Convert pthread_t to thread_act_t via `pthread_mach_thread_np()`

---

### 2. How to suspend threads safely on macOS?

**Decision**: Use `thread_suspend(thread_act_t)` from Mach kernel API.

**Rationale**:
- Kernel-level suspension - thread cannot run until resumed
- Does not require elevated privileges for same-process threads
- Returns immediately (non-blocking)
- Safe to call on any thread including self (though we skip self)

**Alternatives Considered**:

| Alternative | Rejected Because |
|-------------|-----------------|
| Signals (SIGSTOP) | Process-wide, not per-thread |
| pthread_kill + sigsuspend | Complex, signal-safety issues |
| Cooperative yield | Requires instrumentation of target code |

**API Details**:
```c
#include <mach/mach.h>

kern_return_t thread_suspend(thread_act_t target_thread);
kern_return_t thread_resume(thread_act_t target_thread);

// Returns:
// KERN_SUCCESS - operation completed
// KERN_INVALID_ARGUMENT - invalid thread port
// KERN_TERMINATED - thread terminated
```

**Critical Notes**:
- `thread_suspend` increments a suspend count; must match with equal `thread_resume` calls
- Suspended thread may hold locks - minimize suspend duration
- Check return value; thread may have terminated between discovery and suspend
- Skip sampler thread itself (store `mach_thread_self()` at init)

---

### 3. How to read register state from suspended thread?

**Decision**: Use `thread_get_state()` with architecture-specific flavors.

**Rationale**:
- Provides full register context including PC, SP, FP
- Works on suspended threads (unlike reading own registers)
- Officially supported API

**x86_64 Implementation**:
```c
#include <mach/mach.h>
#include <mach/thread_act.h>
#include <mach/i386/thread_status.h>

x86_thread_state64_t state;
mach_msg_type_number_t count = x86_THREAD_STATE64_COUNT;

kern_return_t kr = thread_get_state(
    thread_port,
    x86_THREAD_STATE64,
    (thread_state_t)&state,
    &count
);

if (kr == KERN_SUCCESS) {
    uintptr_t pc = state.__rip;  // Instruction pointer
    uintptr_t fp = state.__rbp;  // Frame pointer
    uintptr_t sp = state.__rsp;  // Stack pointer
}
```

**arm64 Implementation**:
```c
#include <mach/mach.h>
#include <mach/thread_act.h>
#include <mach/arm/thread_status.h>

arm_thread_state64_t state;
mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;

kern_return_t kr = thread_get_state(
    thread_port,
    ARM_THREAD_STATE64,
    (thread_state_t)&state,
    &count
);

if (kr == KERN_SUCCESS) {
    uintptr_t pc = arm_thread_state64_get_pc(state);  // Use accessor macro
    uintptr_t fp = arm_thread_state64_get_fp(state);  // Frame pointer
    uintptr_t sp = arm_thread_state64_get_sp(state);  // Stack pointer
    uintptr_t lr = arm_thread_state64_get_lr(state);  // Link register
}
```

**Notes**:
- arm64 uses accessor macros due to pointer authentication (PAC)
- Both architectures use standard C calling conventions on macOS
- FP register is the starting point for frame walking

---

### 4. How to walk the stack without libunwind?

**Decision**: Direct frame pointer chain walking.

**Rationale**:
- macOS guarantees frame pointers in ABI (especially arm64)
- Direct memory access is faster than libunwind
- Thread is suspended, so memory is stable
- Simple pointer arithmetic

**arm64 Frame Layout** (Apple ABI):
```
Higher addresses (stack grows down)
┌─────────────────────┐
│    ...              │
├─────────────────────┤
│  Return Address     │  FP + 8 (Link Register saved here)
├─────────────────────┤
│  Previous FP        │  FP + 0 (Points to caller's frame)
├─────────────────────┤
│  Saved Registers    │  FP - 8, FP - 16, ...
├─────────────────────┤
│  Local Variables    │
├─────────────────────┤
│    ...              │
└─────────────────────┘
Lower addresses
```

**x86_64 Frame Layout**:
```
Higher addresses
┌─────────────────────┐
│    ...              │
├─────────────────────┤
│  Return Address     │  RBP + 8
├─────────────────────┤
│  Previous RBP       │  RBP + 0
├─────────────────────┤
│  Local Variables    │  RBP - 8, RBP - 16, ...
└─────────────────────┘
Lower addresses
```

**Walking Algorithm**:
```c
typedef struct {
    uintptr_t fp;   // Frame pointer
    uintptr_t lr;   // Return address (link register / return IP)
} StackFrame;

int walk_frames(uintptr_t initial_fp, uintptr_t stack_base, 
                uintptr_t stack_limit, uintptr_t* frames, int max_frames) {
    int count = 0;
    uintptr_t fp = initial_fp;
    
    while (fp != 0 && count < max_frames) {
        // Validate pointer is within stack bounds and aligned
        if (fp < stack_limit || fp >= stack_base || (fp & 0x7) != 0) {
            break;
        }
        
        StackFrame* frame = (StackFrame*)fp;
        uintptr_t return_addr = frame->lr;  // FP + 8
        uintptr_t prev_fp = frame->fp;      // FP + 0
        
        // Store return address (this is the caller's instruction pointer)
        frames[count++] = return_addr;
        
        // Walk to previous frame
        fp = prev_fp;
    }
    
    return count;
}
```

**Stack Bounds**:
```c
#include <pthread.h>

void get_stack_bounds(pthread_t thread, uintptr_t* base, uintptr_t* limit) {
    void* stack_addr = pthread_get_stackaddr_np(thread);
    size_t stack_size = pthread_get_stacksize_np(thread);
    
    *base = (uintptr_t)stack_addr;                    // High address
    *limit = (uintptr_t)stack_addr - stack_size;     // Low address
}
```

---

### 5. How to achieve precise timing for sampling interval?

**Decision**: Use `mach_wait_until()` with `mach_absolute_time()`.

**Rationale**:
- Nanosecond precision timing
- No signal overhead
- Predictable wake-up (not subject to timer coalescing like usleep)

**Implementation**:
```c
#include <mach/mach_time.h>

static mach_timebase_info_data_t g_timebase;

void init_timing(void) {
    mach_timebase_info(&g_timebase);
}

uint64_t ns_to_mach(uint64_t ns) {
    return ns * g_timebase.denom / g_timebase.numer;
}

void sampler_loop(uint64_t interval_ns) {
    uint64_t interval_mach = ns_to_mach(interval_ns);
    uint64_t next_time = mach_absolute_time() + interval_mach;
    
    while (g_sampling_active) {
        mach_wait_until(next_time);
        
        // Sample all threads
        sample_all_threads();
        
        // Schedule next sample (absolute time, not relative)
        next_time += interval_mach;
        
        // Catch up if we fell behind
        uint64_t now = mach_absolute_time();
        if (next_time < now) {
            next_time = now + interval_mach;
        }
    }
}
```

---

### 6. Thread Registry Synchronization

**Decision**: Use `pthread_mutex` with minimal critical section.

**Rationale**:
- Hook callback can fire from any thread
- Sampler needs consistent snapshot
- Lock held only during registry modification
- Copy-out pattern for iteration

**Implementation Pattern**:
```c
typedef struct {
    pthread_mutex_t lock;
    thread_act_t* threads;
    size_t count;
    size_t capacity;
    thread_act_t sampler_thread;  // Exclude from sampling
} ThreadRegistry;

// Called from introspection hook
void registry_add(ThreadRegistry* r, pthread_t pt) {
    thread_act_t mach_thread = pthread_mach_thread_np(pt);
    
    pthread_mutex_lock(&r->lock);
    // Add to array (grow if needed)
    if (r->count >= r->capacity) {
        r->capacity *= 2;
        r->threads = realloc(r->threads, r->capacity * sizeof(thread_act_t));
    }
    r->threads[r->count++] = mach_thread;
    pthread_mutex_unlock(&r->lock);
}

// Called from sampler thread - returns snapshot
size_t registry_snapshot(ThreadRegistry* r, thread_act_t* out, size_t max) {
    pthread_mutex_lock(&r->lock);
    size_t n = r->count < max ? r->count : max;
    memcpy(out, r->threads, n * sizeof(thread_act_t));
    pthread_mutex_unlock(&r->lock);
    return n;
}
```

---

### 7. Converting Native Frames to Python Frames

**Decision**: Resolve Python frames post-walk using `PyGILState_Ensure`.

**Rationale**:
- Cannot access Python objects during thread suspension
- GIL required for safe PyCodeObject access
- Existing resolver thread pattern works here

**Strategy**:
1. During suspend: Capture raw frame pointers (native instruction addresses)
2. After resume: Store in ring buffer as `RawSample`
3. Resolver thread: Match native IPs to Python frames via `PyInterpreterState_Head()` thread iteration

**Note**: For mixed-mode profiling, we need to correlate:
- Native return addresses → Python `PyCodeObject*` via bytecode address ranges
- This requires walking Python frames separately when needed

---

## Platform Requirements

| Requirement | macOS Version | Notes |
|-------------|--------------|-------|
| `pthread_introspection_hook_install` | 10.7+ | Stable since Lion |
| `thread_suspend/resume` | All | Core Mach API |
| `thread_get_state` | All | Core Mach API |
| `mach_wait_until` | All | Mach timing |
| arm64 support | 11.0+ | Apple Silicon |
| Frame pointers guaranteed | arm64 always, x86_64 by convention | ABI requirement |

**Minimum Target**: macOS 10.15 (Catalina) - covers all supported macOS versions.

---

## Risk Assessment

| Risk | Likelihood | Impact | Mitigation |
|------|------------|--------|------------|
| Thread terminated during walk | Medium | Low | Check `thread_get_state` return; skip on KERN_TERMINATED |
| Stack memory paged out | Low | Medium | Validate pointers; short suspend window |
| Lock contention in registry | Low | Low | Short critical sections; snapshot pattern |
| Frame pointer omitted | Low | Low | Rare on macOS; graceful termination of walk |
| API deprecation | Very Low | High | Apple APIs stable; monitor WWDC |

