# Quickstart: Darwin Mach-Based Sampler Implementation

**Feature**: 003-darwin-mach-sampler  
**Date**: 2024-12-01

## Prerequisites

- macOS 10.15+ (Catalina or later)
- Xcode Command Line Tools installed
- Python 3.9+ development headers
- Understanding of existing `platform/` architecture

## Quick Reference

### Key Files to Modify/Create

| File | Action | Purpose |
|------|--------|---------|
| `src/spprof/_ext/platform/darwin_mach.h` | CREATE | Public Mach sampler interface |
| `src/spprof/_ext/platform/darwin_mach.c` | CREATE | Mach sampler implementation |
| `src/spprof/_ext/platform/darwin.c` | MODIFY | Switch to Mach sampler |
| `tests/test_darwin_mach.py` | CREATE | Platform-specific tests |

### Key Mach APIs

```c
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread/introspection.h>

// Thread discovery
pthread_introspection_hook_install(hook);
pthread_mach_thread_np(pthread);

// Thread control
thread_suspend(thread_act_t);
thread_resume(thread_act_t);
thread_get_state(thread, flavor, state, &count);

// Timing
mach_absolute_time();
mach_wait_until(deadline);
mach_timebase_info(&info);

// Stack bounds
pthread_get_stackaddr_np(thread);
pthread_get_stacksize_np(thread);
```

## Implementation Order

### Step 1: Thread Registry (Day 1)

Create the thread tracking infrastructure:

```c
// darwin_mach.c

#include <pthread/introspection.h>

static ThreadRegistry g_registry;
static pthread_introspection_hook_t g_prev_hook = NULL;

static void introspection_hook(unsigned int event, pthread_t thread, 
                                void *addr, size_t size) {
    (void)addr; (void)size;
    
    switch (event) {
        case PTHREAD_INTROSPECTION_THREAD_START:
            registry_add(&g_registry, thread);
            break;
        case PTHREAD_INTROSPECTION_THREAD_TERMINATE:
            registry_remove(&g_registry, thread);
            break;
    }
    
    // Chain to previous hook
    if (g_prev_hook) {
        g_prev_hook(event, thread, addr, size);
    }
}

int mach_sampler_init(void) {
    if (registry_init(&g_registry) != 0) {
        return -1;
    }
    
    g_prev_hook = pthread_introspection_hook_install(introspection_hook);
    return 0;
}
```

**Test**: Verify thread count increases/decreases with `threading.Thread`.

### Step 2: Sampler Thread Loop (Day 2)

Create the precision timing loop:

```c
static void* sampler_thread_func(void* arg) {
    MachSamplerState* state = (MachSamplerState*)arg;
    
    // Store our own thread port (to skip during sampling)
    state->sampler_mach_thread = mach_thread_self();
    
    uint64_t next_time = mach_absolute_time() + state->interval_mach;
    
    while (state->running) {
        // Precise sleep until next sample time
        mach_wait_until(next_time);
        
        // Take snapshot of threads
        ThreadSnapshot snapshot;
        registry_snapshot(&state->registry, &snapshot);
        
        // Sample each thread
        for (size_t i = 0; i < snapshot.count; i++) {
            if (snapshot.entries[i].mach_port == state->sampler_mach_thread) {
                continue;  // Don't sample ourselves
            }
            sample_thread(&snapshot.entries[i], state);
        }
        
        // Schedule next sample
        next_time += state->interval_mach;
        
        // Catch up if we fell behind
        uint64_t now = mach_absolute_time();
        if (next_time < now) {
            next_time = now + state->interval_mach;
        }
    }
    
    return NULL;
}
```

**Test**: Verify samples are taken at correct intervals.

### Step 3: Thread Suspension & Register Capture (Day 2-3)

Implement the suspend-get_state-resume cycle:

```c
static int sample_thread(ThreadEntry* entry, MachSamplerState* state) {
    kern_return_t kr;
    
    // Suspend the thread
    kr = thread_suspend(entry->mach_port);
    if (kr != KERN_SUCCESS) {
        state->stats.threads_skipped++;
        return -1;
    }
    
    uint64_t suspend_start = mach_absolute_time();
    
    // Get register state
    RegisterState regs;
    if (get_register_state(entry->mach_port, &regs) != 0) {
        thread_resume(entry->mach_port);
        state->stats.walk_errors++;
        return -1;
    }
    
    // Walk the stack
    CapturedStack stack = {0};
    stack.thread_id = entry->thread_id;
    stack.timestamp = platform_monotonic_ns();
    
    int depth = walk_stack(entry, &regs, &stack, state->config.max_stack_depth);
    
    // Resume IMMEDIATELY after walking
    kr = thread_resume(entry->mach_port);
    
    uint64_t suspend_end = mach_absolute_time();
    uint64_t suspend_ns = mach_to_ns(suspend_end - suspend_start);
    
    // Update statistics
    state->stats.suspend_time_ns += suspend_ns;
    if (suspend_ns > state->stats.max_suspend_ns) {
        state->stats.max_suspend_ns = suspend_ns;
    }
    
    if (depth > 0) {
        // Write to ring buffer
        write_sample_to_ringbuffer(&stack, state->ringbuffer);
        state->stats.samples_captured++;
    }
    
    state->stats.threads_sampled++;
    return 0;
}
```

**Test**: Verify thread is suspended for <100μs.

### Step 4: Stack Walking (Day 3-4)

Implement frame pointer chain walking:

```c
int walk_stack(const ThreadEntry* entry, const RegisterState* regs,
               CapturedStack* stack, int max_depth) {
    
    uintptr_t fp = regs->fp;
    uintptr_t stack_base = entry->stack_base;
    uintptr_t stack_limit = entry->stack_limit;
    
    // First frame: current PC
    if (regs->pc != 0 && stack->depth < max_depth) {
        stack->frames[stack->depth].return_addr = regs->pc;
        stack->frames[stack->depth].frame_ptr = fp;
        stack->depth++;
    }
    
    // Walk frame pointer chain
    while (fp != 0 && stack->depth < max_depth) {
        // Validate frame pointer
        if (!validate_frame_pointer(fp, stack_base, stack_limit)) {
            break;
        }
        
        // Read frame (FP points to saved FP, FP+8 is return address)
        uintptr_t* frame = (uintptr_t*)fp;
        uintptr_t prev_fp = frame[0];
        uintptr_t return_addr = frame[1];
        
        if (return_addr == 0) {
            break;  // Bottom of stack
        }
        
        stack->frames[stack->depth].return_addr = return_addr;
        stack->frames[stack->depth].frame_ptr = prev_fp;
        stack->depth++;
        
        // Move to previous frame
        fp = prev_fp;
    }
    
    if (stack->depth >= max_depth && fp != 0) {
        stack->truncated = 1;
    }
    
    return stack->depth;
}

int validate_frame_pointer(uintptr_t fp, uintptr_t stack_base, uintptr_t stack_limit) {
    // Check bounds
    if (fp < stack_limit || fp >= stack_base) {
        return 0;
    }
    // Check alignment (8-byte on both x86_64 and arm64)
    if ((fp & 0x7) != 0) {
        return 0;
    }
    // Ensure we have room to read frame (prev_fp + return_addr = 16 bytes)
    if (fp + 16 > stack_base) {
        return 0;
    }
    return 1;
}
```

**Test**: Verify correct stack depth captured.

### Step 5: Architecture Support (Day 4)

Add x86_64 and arm64 register extraction:

```c
static int get_register_state(thread_act_t thread, RegisterState* out) {
    kern_return_t kr;
    
#if defined(__x86_64__)
    x86_thread_state64_t state;
    mach_msg_type_number_t count = x86_THREAD_STATE64_COUNT;
    
    kr = thread_get_state(thread, x86_THREAD_STATE64, 
                          (thread_state_t)&state, &count);
    if (kr != KERN_SUCCESS) {
        return -1;
    }
    
    out->pc = state.__rip;
    out->sp = state.__rsp;
    out->fp = state.__rbp;
    out->lr = 0;
    
#elif defined(__arm64__)
    arm_thread_state64_t state;
    mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
    
    kr = thread_get_state(thread, ARM_THREAD_STATE64,
                          (thread_state_t)&state, &count);
    if (kr != KERN_SUCCESS) {
        return -1;
    }
    
    out->pc = arm_thread_state64_get_pc(state);
    out->sp = arm_thread_state64_get_sp(state);
    out->fp = arm_thread_state64_get_fp(state);
    out->lr = arm_thread_state64_get_lr(state);
#else
    #error "Unsupported architecture"
#endif
    
    return 0;
}
```

### Step 6: Platform Integration (Day 5)

Update `darwin.c` to use Mach sampler:

```c
// darwin.c

#include "darwin_mach.h"

int platform_init(void) {
    return mach_sampler_init();
}

void platform_cleanup(void) {
    mach_sampler_cleanup();
}

int platform_timer_create(uint64_t interval_ns) {
    return mach_sampler_start(interval_ns, g_ringbuffer);
}

int platform_timer_destroy(void) {
    return mach_sampler_stop();
}

void platform_get_stats(uint64_t* captured, uint64_t* dropped, uint64_t* overruns) {
    mach_sampler_get_stats(captured, dropped, NULL);
    if (overruns) *overruns = 0;  // Not applicable for Mach approach
}
```

## Testing Strategy

### Unit Tests (C level)

```c
// Test thread registry
void test_registry_add_remove(void) {
    ThreadRegistry reg;
    registry_init(&reg);
    
    pthread_t pt = pthread_self();
    registry_add(&reg, pt);
    assert(reg.count == 1);
    
    registry_remove(&reg, pt);
    // Entry marked invalid, not removed yet
    
    registry_compact(&reg);
    assert(reg.count == 0);
    
    registry_cleanup(&reg);
}

// Test frame pointer validation
void test_validate_fp(void) {
    uintptr_t base = 0x7fff00010000;
    uintptr_t limit = 0x7fff00000000;
    
    // Valid
    assert(validate_frame_pointer(0x7fff00008000, base, limit) == 1);
    
    // Out of bounds
    assert(validate_frame_pointer(0x7fff00020000, base, limit) == 0);
    assert(validate_frame_pointer(0x7ffefffff000, base, limit) == 0);
    
    // Misaligned
    assert(validate_frame_pointer(0x7fff00008001, base, limit) == 0);
}
```

### Integration Tests (Python level)

```python
# test_darwin_mach.py

import platform
import pytest
import threading
import time

# Skip on non-Darwin
pytestmark = pytest.mark.skipif(
    platform.system() != 'Darwin',
    reason="Darwin-only tests"
)

def test_multithread_sampling():
    """Verify samples from all threads."""
    import spprof
    
    results = {}
    
    def worker(name):
        total = 0
        for i in range(1000000):
            total += i
        results[name] = total
    
    threads = [threading.Thread(target=worker, args=(f"t{i}",)) for i in range(4)]
    
    with spprof.Profiler(interval_ms=10) as p:
        for t in threads:
            t.start()
        for t in threads:
            t.join()
    
    # Check samples from multiple threads
    samples = p.get_samples()
    thread_ids = set(s.thread_id for s in samples)
    
    # Should have samples from more than just main thread
    assert len(thread_ids) >= 2, f"Expected samples from multiple threads, got {thread_ids}"

def test_sampling_rate_accuracy():
    """Verify sampling rate is within 10% of configured."""
    import spprof
    
    duration = 2.0  # seconds
    interval_ms = 10
    expected_samples = duration * (1000 / interval_ms)
    
    with spprof.Profiler(interval_ms=interval_ms) as p:
        time.sleep(duration)
    
    actual_samples = len(p.get_samples())
    accuracy = actual_samples / expected_samples
    
    assert 0.9 <= accuracy <= 1.1, f"Expected ~{expected_samples}, got {actual_samples}"
```

## Common Pitfalls

### 1. Suspend Count Mismatch
```c
// WRONG: Unmatched suspend/resume
thread_suspend(t);
if (error) return;  // Resume never called!

// RIGHT: Always resume
thread_suspend(t);
// ... do work ...
thread_resume(t);  // Always called
```

### 2. Stack Bounds on Main Thread
```c
// Main thread may have unusual stack setup
// Always check return values
void* addr = pthread_get_stackaddr_np(thread);
if (addr == NULL) {
    // Handle gracefully
}
```

### 3. Thread Termination Race
```c
// Thread may terminate between snapshot and suspend
kr = thread_suspend(entry->mach_port);
if (kr == KERN_TERMINATED || kr == KERN_INVALID_ARGUMENT) {
    // Thread already gone - this is normal, not an error
    return 0;
}
```

### 4. arm64 Pointer Authentication
```c
// WRONG: Direct register access on arm64
uintptr_t pc = state.__pc;  // May have PAC bits!

// RIGHT: Use accessor macros
uintptr_t pc = arm_thread_state64_get_pc(state);
```

## Build & Test Commands

```bash
# Build the extension
cd /Users/rkvasnytskyi/projects/spprof
pip install -e .

# Run Darwin-specific tests
pytest tests/test_darwin_mach.py -v

# Run with verbose logging
SPPROF_DEBUG=1 pytest tests/test_darwin_mach.py -v

# Build with debug symbols
CFLAGS="-g -O0 -DSPPROF_DEBUG" pip install -e .
```

## References

- [Mach Kernel Interfaces](https://developer.apple.com/documentation/kernel/mach)
- [pthread_introspection](https://opensource.apple.com/source/libpthread/)
- [Apple ARM64 ABI](https://developer.apple.com/documentation/xcode/writing-arm64-code-for-apple-platforms)
- [Thread State Flavors](https://opensource.apple.com/source/xnu/xnu-7195.81.3/osfmk/mach/thread_status.h)

