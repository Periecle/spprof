/**
 * darwin_mach.c - Darwin Mach-based sampler implementation
 *
 * Implements the "Suspend-Walk-Resume" sampling pattern using Mach kernel APIs:
 *   - pthread_introspection_hook_install for thread discovery
 *   - thread_suspend/thread_resume for safe thread stopping
 *   - thread_get_state for register capture
 *   - Direct frame pointer walking for stack traces
 *   - mach_wait_until for precise timing
 *
 * FREE-THREADING SAFETY (Py_GIL_DISABLED):
 *
 * This implementation is SAFE for free-threaded Python builds because it uses
 * thread suspension (thread_suspend/thread_resume) to ensure the target thread's
 * state is stable during frame walking.
 *
 * Unlike signal-based sampling (SIGPROF), which interrupts a thread at an
 * arbitrary point but allows it to continue executing, Mach thread suspension
 * FULLY STOPS the target thread. This means:
 *
 *   1. FRAME CHAIN STABILITY:
 *      The frame->previous pointers cannot change while suspended.
 *      Function calls/returns are frozen mid-execution.
 *
 *   2. REGISTER STATE STABILITY:
 *      PC, SP, FP are captured via thread_get_state() while thread is stopped.
 *      No race condition with the thread modifying these registers.
 *
 *   3. SAFE REFERENCE COUNTING:
 *      We acquire the GIL (via PyGILState_Ensure) before accessing Python
 *      objects. In free-threaded builds, this acquires the critical section.
 *      Py_INCREF/Py_DECREF are safe with the critical section held.
 *
 *   4. SAFE THREAD STATE ITERATION:
 *      PyInterpreterState_ThreadHead/PyThreadState_Next are called with GIL held.
 *
 * The only platform requirement is that thread_suspend() actually stops the
 * thread (Mach kernel guarantee) and doesn't just deliver a signal.
 *
 * Copyright (c) 2024 spprof contributors
 * SPDX-License-Identifier: MIT
 */

#ifdef __APPLE__

#include <Python.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_act.h>
#include <pthread.h>
#include <pthread/introspection.h>
#include <stdatomic.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>

#include "darwin_mach.h"
#include "../ringbuffer.h"
#include "../code_registry.h"
#include "../error.h"

/* Internal API for Python frame capture */
#ifdef SPPROF_USE_INTERNAL_API
#include "../internal/pycore_tstate.h"
#endif

/* Architecture-specific includes */
#if defined(__x86_64__)
#include <mach/i386/thread_status.h>
#elif defined(__arm64__)
#include <mach/arm/thread_status.h>
#else
#error "Unsupported architecture - only x86_64 and arm64 are supported"
#endif

/*
 * =============================================================================
 * Debug Logging (Async-Signal-Safe)
 * =============================================================================
 * CRITICAL: fprintf is NOT async-signal-safe. If a target thread is suspended
 * while holding the stdio lock, and the sampler thread attempts to log using
 * fprintf, the process will deadlock. We use write(STDERR_FILENO, ...) which
 * is async-signal-safe.
 */

#ifdef SPPROF_DEBUG
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>

/* Async-signal-safe debug logging using write() instead of fprintf() */
static void mach_debug_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void mach_debug_log(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf) - 1, fmt, args);
    va_end(args);
    if (len > 0) {
        if ((size_t)len >= sizeof(buf) - 1) {
            len = sizeof(buf) - 2;
        }
        buf[len] = '\n';
        buf[len + 1] = '\0';
        /* write() is async-signal-safe, unlike fprintf */
        (void)write(STDERR_FILENO, "[mach_sampler] ", 15);
        (void)write(STDERR_FILENO, buf, len + 1);
    }
}
#define MACH_DEBUG(fmt, ...) mach_debug_log(fmt, ##__VA_ARGS__)
#else
/* Accept any arguments but discard them - avoids C23 variadic warning */
#define MACH_DEBUG(...) ((void)0)
#endif

/*
 * =============================================================================
 * Constants
 * =============================================================================
 */

/* Maximum threads to sample in one cycle */
#define MACH_MAX_THREADS_PER_SAMPLE 256

/* Initial thread registry capacity */
#define REGISTRY_INITIAL_CAPACITY 32

/* Maximum thread registry capacity */
#define REGISTRY_MAX_CAPACITY 4096

/* Minimum sampling interval (1ms) */
#define MIN_INTERVAL_NS 1000000ULL

/* Maximum sampling interval (1s) */
#define MAX_INTERVAL_NS 1000000000ULL

/* Maximum stack depth - match ringbuffer.h */
#ifndef SPPROF_MAX_STACK_DEPTH
#define SPPROF_MAX_STACK_DEPTH 128
#endif

/*
 * Frame record size for stack walking.
 * On both x86_64 and arm64, each stack frame contains:
 *   - Previous frame pointer (8 bytes)
 *   - Return address (8 bytes)
 * Total: 16 bytes = 2 * sizeof(void*)
 */
#define FRAME_RECORD_SIZE (2 * sizeof(void*))

/*
 * =============================================================================
 * Data Structures (T004-T012)
 * =============================================================================
 */

/**
 * Unified register state (architecture-independent view)
 */
typedef struct {
    uintptr_t pc;       /* Program counter (RIP on x86, PC on arm64) */
    uintptr_t sp;       /* Stack pointer (RSP on x86, SP on arm64) */
    uintptr_t fp;       /* Frame pointer (RBP on x86, FP on arm64) */
    uintptr_t lr;       /* Link register (arm64 only, 0 on x86) */
} RegisterState;

/**
 * Single captured frame
 */
typedef struct {
    uintptr_t return_addr;      /* Return address (caller's IP) */
    uintptr_t frame_ptr;        /* Frame pointer value */
} CapturedFrame;

/**
 * Captured stack from one thread
 */
typedef struct {
    CapturedFrame frames[SPPROF_MAX_STACK_DEPTH];
    int depth;
    uint64_t thread_id;
    uint64_t timestamp;
    int truncated;              /* 1 if stack deeper than max */
    int error;                  /* Non-zero on walk error */
} CapturedStack;

/**
 * Thread entry in the registry
 */
typedef struct {
    thread_act_t mach_port;     /* Mach thread port for suspend/resume/get_state */
    pthread_t pthread;          /* Original pthread handle */
    uint64_t thread_id;         /* Cached OS thread ID for RawSample */
    uintptr_t stack_base;       /* Stack high address (from pthread) */
    uintptr_t stack_limit;      /* Stack low address (from pthread) */
    int is_valid;               /* 0 if thread has terminated */
} ThreadEntry;

/**
 * Thread registry - tracks all active threads in the process
 */
typedef struct {
    pthread_mutex_t lock;           /* Protects all fields */
    ThreadEntry* entries;           /* Dynamic array of thread entries */
    size_t count;                   /* Number of valid entries */
    size_t capacity;                /* Allocated capacity */
    thread_act_t sampler_thread;    /* Mach port of sampler (excluded from sampling) */
    int hook_installed;             /* 1 if introspection hook active */
    pthread_introspection_hook_t prev_hook;  /* Previous hook to chain */
} ThreadRegistry;

/**
 * Snapshot of threads to sample (copied from registry)
 */
typedef struct {
    ThreadEntry entries[MACH_MAX_THREADS_PER_SAMPLE];
    size_t count;
} ThreadSnapshot;

/**
 * Sampler configuration
 */
typedef struct {
    uint64_t interval_ns;           /* Sampling interval in nanoseconds */
    int native_unwinding;           /* 1 to capture native frames */
    int max_stack_depth;            /* Maximum frames to capture */
} MachSamplerConfig;

/**
 * Sampler statistics
 *
 * Uses _Atomic types for proper concurrent access between:
 * - Writer: Sampler thread (single writer)
 * - Readers: Any thread calling mach_sampler_get_stats()
 *
 * We use relaxed memory ordering since these are just counters
 * with no synchronization dependencies.
 */
typedef struct {
    _Atomic uint64_t samples_captured;     /* Successful samples written */
    _Atomic uint64_t samples_dropped;      /* Samples dropped (buffer full) */
    _Atomic uint64_t threads_sampled;      /* Total thread samples taken */
    _Atomic uint64_t threads_skipped;      /* Threads skipped (terminated, invalid) */
    _Atomic uint64_t suspend_time_ns;      /* Total time threads were suspended */
    _Atomic uint64_t max_suspend_ns;       /* Maximum single suspension time */
    _Atomic uint64_t walk_errors;          /* Frame walking errors */
} MachSamplerStats;

/**
 * Main sampler state
 */
typedef struct {
    /* Configuration */
    MachSamplerConfig config;
    
    /* Timing */
    mach_timebase_info_data_t timebase;
    uint64_t interval_mach;             /* Interval in mach_absolute_time units */
    
    /* Thread management */
    pthread_t sampler_pthread;          /* Sampler thread handle */
    thread_act_t sampler_mach_thread;   /* Sampler's own Mach port */
    _Atomic int running;                /* 1 while sampler should run */
    _Atomic int initialized;            /* 1 if initialized */
    
    /* Thread registry */
    ThreadRegistry registry;
    
    /* Output */
    RingBuffer* ringbuffer;             /* Existing ring buffer (shared) */
    
    /* Statistics */
    MachSamplerStats stats;
} MachSamplerState;

/*
 * =============================================================================
 * Global State
 * =============================================================================
 */

static MachSamplerState g_state = {0};

/*
 * =============================================================================
 * Timing Functions (T013)
 * =============================================================================
 */

/**
 * Convert nanoseconds to mach_absolute_time units.
 */
static uint64_t ns_to_mach(uint64_t ns) {
    return ns * g_state.timebase.denom / g_state.timebase.numer;
}

/**
 * Convert mach_absolute_time units to nanoseconds.
 */
static uint64_t mach_to_ns(uint64_t mach_time) {
    return mach_time * g_state.timebase.numer / g_state.timebase.denom;
}

/*
 * =============================================================================
 * Thread Registry Operations (T006-T007, T014-T017)
 * =============================================================================
 */

/**
 * Initialize the thread registry.
 */
static int registry_init(ThreadRegistry* registry) {
    int ret;
    
    memset(registry, 0, sizeof(ThreadRegistry));
    
    ret = pthread_mutex_init(&registry->lock, NULL);
    if (ret != 0) {
        errno = ret;
        return -1;
    }
    
    registry->entries = malloc(REGISTRY_INITIAL_CAPACITY * sizeof(ThreadEntry));
    if (!registry->entries) {
        pthread_mutex_destroy(&registry->lock);
        errno = ENOMEM;
        return -1;
    }
    
    registry->capacity = REGISTRY_INITIAL_CAPACITY;
    registry->count = 0;
    registry->hook_installed = 0;
    registry->prev_hook = NULL;
    registry->sampler_thread = MACH_PORT_NULL;
    
    return 0;
}

/**
 * Clean up the thread registry.
 */
static void registry_cleanup(ThreadRegistry* registry) {
    pthread_mutex_lock(&registry->lock);
    
    if (registry->entries) {
        free(registry->entries);
        registry->entries = NULL;
    }
    registry->count = 0;
    registry->capacity = 0;
    
    pthread_mutex_unlock(&registry->lock);
    pthread_mutex_destroy(&registry->lock);
}

/**
 * Add a thread to the registry.
 *
 * NOTE: We store the pthread_t value (cast to uint64_t) as the thread_id,
 * NOT the result of pthread_threadid_np(). This is because Python's
 * PyThreadState.thread_id uses PyThread_get_thread_ident() which returns
 * (unsigned long)pthread_self() on macOS. We need to match this format
 * for thread lookup in sample_all_threads().
 */
static void registry_add(ThreadRegistry* registry, pthread_t pthread_handle) {
    thread_act_t mach_thread = pthread_mach_thread_np(pthread_handle);
    
    if (mach_thread == MACH_PORT_NULL) {
        return;
    }
    
    /* Skip the sampler thread itself */
    if (mach_thread == registry->sampler_thread) {
        return;
    }
    
    /* Use pthread_t cast to match Python's tstate->thread_id format.
     * Python uses PyThread_get_thread_ident() which is (unsigned long)pthread_self() */
    uint64_t thread_id = (uint64_t)(unsigned long)pthread_handle;
    
    /* Get stack bounds */
    void* stack_addr = pthread_get_stackaddr_np(pthread_handle);
    size_t stack_size = pthread_get_stacksize_np(pthread_handle);
    
    uintptr_t stack_base = (uintptr_t)stack_addr;
    uintptr_t stack_limit = stack_base - stack_size;
    
    pthread_mutex_lock(&registry->lock);
    
    /* Check for duplicates */
    for (size_t i = 0; i < registry->count; i++) {
        if (registry->entries[i].mach_port == mach_thread) {
            /* Already registered, just mark valid */
            registry->entries[i].is_valid = 1;
            pthread_mutex_unlock(&registry->lock);
            return;
        }
    }
    
    /* Grow if needed */
    if (registry->count >= registry->capacity) {
        if (registry->capacity >= REGISTRY_MAX_CAPACITY) {
            /* At max capacity, skip */
            pthread_mutex_unlock(&registry->lock);
            return;
        }
        
        size_t new_capacity = registry->capacity * 2;
        if (new_capacity > REGISTRY_MAX_CAPACITY) {
            new_capacity = REGISTRY_MAX_CAPACITY;
        }
        
        ThreadEntry* new_entries = realloc(registry->entries, 
                                           new_capacity * sizeof(ThreadEntry));
        if (!new_entries) {
            pthread_mutex_unlock(&registry->lock);
            return;
        }
        
        registry->entries = new_entries;
        registry->capacity = new_capacity;
    }
    
    /* Add the new entry */
    ThreadEntry* entry = &registry->entries[registry->count];
    entry->mach_port = mach_thread;
    entry->pthread = pthread_handle;
    entry->thread_id = thread_id;
    entry->stack_base = stack_base;
    entry->stack_limit = stack_limit;
    entry->is_valid = 1;
    
    registry->count++;
    
    pthread_mutex_unlock(&registry->lock);
}

/**
 * Mark a thread as terminated in the registry.
 */
static void registry_remove(ThreadRegistry* registry, pthread_t pthread_handle) {
    thread_act_t mach_thread = pthread_mach_thread_np(pthread_handle);
    
    pthread_mutex_lock(&registry->lock);
    
    for (size_t i = 0; i < registry->count; i++) {
        if (registry->entries[i].pthread == pthread_handle ||
            registry->entries[i].mach_port == mach_thread) {
            registry->entries[i].is_valid = 0;
            break;
        }
    }
    
    pthread_mutex_unlock(&registry->lock);
}

/**
 * Get a snapshot of threads for sampling.
 */
static void registry_snapshot(ThreadRegistry* registry, ThreadSnapshot* snapshot) {
    pthread_mutex_lock(&registry->lock);
    
    snapshot->count = 0;
    
    for (size_t i = 0; i < registry->count && snapshot->count < MACH_MAX_THREADS_PER_SAMPLE; i++) {
        if (registry->entries[i].is_valid) {
            snapshot->entries[snapshot->count] = registry->entries[i];
            snapshot->count++;
        }
    }
    
    pthread_mutex_unlock(&registry->lock);
}

/**
 * Compact registry by removing invalid entries.
 */
static void registry_compact(ThreadRegistry* registry) {
    pthread_mutex_lock(&registry->lock);
    
    size_t write_idx = 0;
    for (size_t read_idx = 0; read_idx < registry->count; read_idx++) {
        if (registry->entries[read_idx].is_valid) {
            if (write_idx != read_idx) {
                registry->entries[write_idx] = registry->entries[read_idx];
            }
            write_idx++;
        }
    }
    
    registry->count = write_idx;
    
    pthread_mutex_unlock(&registry->lock);
}

/*
 * =============================================================================
 * Pthread Introspection Hook (T018)
 * =============================================================================
 */

/**
 * Introspection hook callback for thread lifecycle events.
 */
static void introspection_hook(unsigned int event, pthread_t thread, 
                               void *addr, size_t size) {
    (void)addr;
    (void)size;
    
    switch (event) {
        case PTHREAD_INTROSPECTION_THREAD_START:
            registry_add(&g_state.registry, thread);
            break;
        case PTHREAD_INTROSPECTION_THREAD_TERMINATE:
            registry_remove(&g_state.registry, thread);
            break;
        /* Ignore CREATE and DESTROY - use START/TERMINATE for safety */
    }
    
    /* Chain to previous hook if any */
    if (g_state.registry.prev_hook) {
        g_state.registry.prev_hook(event, thread, addr, size);
    }
}

/*
 * =============================================================================
 * Register State Extraction (T053-T055)
 * =============================================================================
 */

#if defined(__x86_64__)
/**
 * Get register state from x86_64 thread.
 */
static int get_register_state_x86_64(thread_act_t thread, RegisterState* out) {
    x86_thread_state64_t state;
    mach_msg_type_number_t count = x86_THREAD_STATE64_COUNT;
    
    kern_return_t kr = thread_get_state(thread, x86_THREAD_STATE64,
                                        (thread_state_t)&state, &count);
    if (kr != KERN_SUCCESS) {
        return -1;
    }
    
    out->pc = state.__rip;
    out->sp = state.__rsp;
    out->fp = state.__rbp;
    out->lr = 0;  /* x86 uses stack for return addresses */
    
    return 0;
}
#endif /* __x86_64__ */

#if defined(__arm64__)
/**
 * Get register state from arm64 thread.
 */
static int get_register_state_arm64(thread_act_t thread, RegisterState* out) {
    arm_thread_state64_t state;
    mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
    
    kern_return_t kr = thread_get_state(thread, ARM_THREAD_STATE64,
                                        (thread_state_t)&state, &count);
    if (kr != KERN_SUCCESS) {
        return -1;
    }
    
    /* Use accessor macros for pointer authentication */
    out->pc = arm_thread_state64_get_pc(state);
    out->sp = arm_thread_state64_get_sp(state);
    out->fp = arm_thread_state64_get_fp(state);
    out->lr = arm_thread_state64_get_lr(state);
    
    return 0;
}
#endif /* __arm64__ */

/**
 * Get register state from thread (architecture dispatch).
 */
static int get_register_state(thread_act_t thread, RegisterState* out) {
#if defined(__x86_64__)
    return get_register_state_x86_64(thread, out);
#elif defined(__arm64__)
    return get_register_state_arm64(thread, out);
#else
    #error "Unsupported architecture"
#endif
}

/*
 * =============================================================================
 * Stack Walking (T021-T022)
 * =============================================================================
 */

/**
 * Validate a frame pointer.
 */
static int validate_frame_pointer(uintptr_t fp, uintptr_t stack_base, 
                                  uintptr_t stack_limit) {
    /* Check bounds */
    if (fp < stack_limit || fp >= stack_base) {
        return 0;
    }
    
    /* Check alignment (8-byte on both x86_64 and arm64) */
    if ((fp & 0x7) != 0) {
        return 0;
    }
    
    /* Ensure we have room to read frame (prev_fp + return_addr) */
    if (fp + FRAME_RECORD_SIZE > stack_base) {
        return 0;
    }
    
    return 1;
}

/**
 * Walk the stack of a suspended thread.
 */
static int walk_stack(const ThreadEntry* entry, const RegisterState* regs,
                      CapturedStack* stack, int max_depth) {
    uintptr_t fp = regs->fp;
    uintptr_t stack_base = entry->stack_base;
    uintptr_t stack_limit = entry->stack_limit;
    
    stack->depth = 0;
    stack->truncated = 0;
    stack->error = 0;
    
    /* First frame: current PC */
    if (regs->pc != 0 && stack->depth < max_depth) {
        stack->frames[stack->depth].return_addr = regs->pc;
        stack->frames[stack->depth].frame_ptr = fp;
        stack->depth++;
    }
    
    /* Walk frame pointer chain */
    while (fp != 0 && stack->depth < max_depth) {
        /* Validate frame pointer */
        if (!validate_frame_pointer(fp, stack_base, stack_limit)) {
            break;
        }
        
        /* Read frame (FP points to saved FP, FP+8 is return address) */
        uintptr_t* frame = (uintptr_t*)fp;
        uintptr_t prev_fp = frame[0];
        uintptr_t return_addr = frame[1];
        
        if (return_addr == 0) {
            break;  /* Bottom of stack */
        }
        
        stack->frames[stack->depth].return_addr = return_addr;
        stack->frames[stack->depth].frame_ptr = prev_fp;
        stack->depth++;
        
        /* Move to previous frame */
        fp = prev_fp;
    }
    
    if (stack->depth >= max_depth && fp != 0) {
        stack->truncated = 1;
    }
    
    return stack->depth;
}

/*
 * =============================================================================
 * Sample Writing (T024)
 * =============================================================================
 */

/**
 * Write a mixed-mode sample (Python + Native frames) to the ring buffer.
 *
 * This captures both the Python frame stack and the native C call stack.
 * Symbol resolution happens later in the resolver to avoid loader lock
 * issues (dladdr is not safe to call with thread suspended).
 */
static int write_mixed_sample_to_ringbuffer(
    uint64_t thread_id,
    uint64_t timestamp,
    uintptr_t* python_frames,
    uintptr_t* instr_ptrs,
    int python_depth,
    const CapturedStack* native_stack,
    RingBuffer* ringbuffer
) {
    RawSample sample;
    
    memset(&sample, 0, sizeof(sample));
    sample.timestamp = timestamp;
    sample.thread_id = thread_id;
    sample.depth = python_depth;
    sample.native_depth = native_stack ? native_stack->depth : 0;
    
    /* Copy Python frame data */
    for (int i = 0; i < python_depth && i < SPPROF_MAX_STACK_DEPTH; i++) {
        sample.frames[i] = python_frames[i];
        sample.instr_ptrs[i] = instr_ptrs ? instr_ptrs[i] : 0;
    }
    
    /* Copy native PC addresses (raw - resolved later via dladdr) */
    if (native_stack) {
        for (int i = 0; i < native_stack->depth && i < SPPROF_MAX_STACK_DEPTH; i++) {
            sample.native_pcs[i] = native_stack->frames[i].return_addr;
        }
    }
    
    return ringbuffer_write(ringbuffer, &sample);
}

/**
 * Write a captured Python stack to the ring buffer (legacy, no native frames).
 */
SPPROF_UNUSED
static int write_python_sample_to_ringbuffer(
    uint64_t thread_id,
    uint64_t timestamp,
    uintptr_t* frames,
    uintptr_t* instr_ptrs,
    int depth,
    RingBuffer* ringbuffer
) {
    return write_mixed_sample_to_ringbuffer(
        thread_id, timestamp, frames, instr_ptrs, depth, NULL, ringbuffer);
}

/**
 * Write a captured native stack to the ring buffer (for native-only mode).
 */
SPPROF_UNUSED
static int write_native_sample_to_ringbuffer(const CapturedStack* stack, 
                                             RingBuffer* ringbuffer) {
    RawSample sample;
    
    memset(&sample, 0, sizeof(sample));
    sample.timestamp = stack->timestamp;
    sample.thread_id = stack->thread_id;
    sample.depth = stack->depth;
    
    /* Copy native instruction pointers - mark with high bit for native frames */
    for (int i = 0; i < stack->depth && i < SPPROF_MAX_STACK_DEPTH; i++) {
        sample.frames[i] = stack->frames[i].return_addr | 0x8000000000000000ULL;
        sample.instr_ptrs[i] = 0;
    }
    
    return ringbuffer_write(ringbuffer, &sample);
}

/*
 * =============================================================================
 * Thread Sampling (T023)
 * =============================================================================
 */

/**
 * Sample all threads - captures both Python and Native frames.
 *
 * MIXED-MODE PROFILING APPROACH:
 * 1. Acquire GIL to safely iterate Python thread states
 * 2. For each thread:
 *    a. Find PyThreadState (requires GIL for safe iteration)
 *    b. Suspend the thread via thread_suspend()
 *    c. Get register state (PC, SP, FP) for native stack walking
 *    d. Walk native C stack via frame pointers (capture PCs only - no symbol resolution)
 *    e. Walk Python frame chain from PyThreadState
 *    f. INCREF code objects to prevent GC (requires GIL)
 *    g. Resume the thread immediately
 *    h. Write sample to ring buffer
 * 3. Release GIL
 *
 * FREE-THREADING SAFETY (Py_GIL_DISABLED):
 *
 * This function is SAFE for free-threaded Python builds because:
 *
 *   1. THREAD SUSPENSION:
 *      thread_suspend() fully stops the target thread before we read any data.
 *      The frame chain cannot change while the thread is suspended.
 *
 *   2. GIL/CRITICAL SECTION:
 *      PyGILState_Ensure() acquires the appropriate synchronization:
 *      - GIL-enabled: Acquires the GIL
 *      - Free-threaded: Acquires the runtime critical section
 *      Either way, thread state iteration and Py_INCREF are safe.
 *
 *   3. ORDER OF OPERATIONS:
 *      We suspend THEN read, and INCREF while suspended + GIL held.
 *      This ensures the captured pointers are valid and won't be freed.
 *
 * DESIGN NOTE ON GIL HOLD TIME:
 * The GIL is held for the entire sampling loop. This is intentional and necessary
 * for safety - NOT a bug or oversight:
 *
 *   1. PyInterpreterState_ThreadHead/PyThreadState_Next require GIL for safe
 *      linked list traversal.
 *
 *   2. code_registry_add_refs_batch() calls Py_INCREF which requires GIL.
 *      We CANNOT defer this because:
 *      - Code objects might be freed by GC between capture and INCREF
 *      - The INCREF must happen while thread is suspended to ensure the
 *        captured pointer is still valid
 *
 *   3. Caching PyThreadState pointers and releasing GIL would be UNSAFE:
 *      - Thread could exit between cache and suspend → dangling pointer
 *      - Even if thread_suspend fails (KERN_TERMINATED), we'd have stale data
 *
 * PERFORMANCE CHARACTERISTICS:
 *   - Per-thread overhead: ~10-50μs (suspend + walk + resume)
 *   - GIL hold time for N threads: ~N × 30μs
 *   - For 20 threads: ~600μs GIL hold time (6% of 10ms interval)
 *   - For 50 threads: ~1.5ms GIL hold time (15% of 10ms interval)
 *
 * For high-thread-count workloads, consider increasing the sampling interval.
 * The overhead scales linearly with thread count.
 *
 * CRITICAL: Symbol resolution (dladdr) is NOT done here to avoid loader lock.
 * Raw PC addresses are captured and resolved later in the resolver.
 */
static void sample_all_threads(ThreadSnapshot* snapshot, MachSamplerState* state) {
    /*
     * Acquire GIL to safely access Python thread states.
     *
     * We need the GIL for:
     * 1. Safe iteration of Python's thread state list
     * 2. Calling Py_INCREF on captured code objects
     *
     * Individual threads are only suspended briefly (~10-50μs) for stack capture.
     * Python bytecode execution is blocked for all threads, but C extensions that
     * release the GIL can continue running.
     */
    PyGILState_STATE gstate = PyGILState_Ensure();
    
    uint64_t sample_start = mach_absolute_time();
    uint64_t timestamp_ns = mach_to_ns(sample_start);
    
    /* Get main interpreter for thread lookup */
    PyInterpreterState* interp = PyInterpreterState_Main();
    if (interp == NULL) {
        PyGILState_Release(gstate);
        return;
    }
    
    for (size_t i = 0; i < snapshot->count; i++) {
        ThreadEntry* entry = &snapshot->entries[i];
        kern_return_t kr;
        
        /* Skip sampler thread */
        if (entry->mach_port == state->sampler_mach_thread) {
            continue;
        }
        
        /* Find PyThreadState for this thread.
         * This iteration requires GIL for safe linked list traversal. */
        PyThreadState* tstate = NULL;
        PyThreadState* ts = PyInterpreterState_ThreadHead(interp);
        while (ts != NULL) {
            if ((uint64_t)ts->thread_id == entry->thread_id) {
                tstate = ts;
                break;
            }
            ts = PyThreadState_Next(ts);
        }
        
        if (tstate == NULL) {
            /* Thread not registered with Python - skip */
            atomic_fetch_add_explicit(&state->stats.threads_skipped, 1, memory_order_relaxed);
            continue;
        }
        
        /* Suspend the thread */
        kr = thread_suspend(entry->mach_port);
        if (kr != KERN_SUCCESS) {
            if (kr == KERN_TERMINATED || kr == KERN_INVALID_ARGUMENT) {
                entry->is_valid = 0;
            }
            atomic_fetch_add_explicit(&state->stats.threads_skipped, 1, memory_order_relaxed);
            continue;
        }
        
        uint64_t suspend_start = mach_absolute_time();
        
        /* ================================================================
         * PHASE 1: Capture Native Stack (raw PC addresses only)
         * ================================================================
         * We capture raw instruction pointers here. Symbol resolution
         * via dladdr() happens AFTER thread_resume() to avoid loader lock.
         */
        CapturedStack native_stack;
        memset(&native_stack, 0, sizeof(native_stack));
        native_stack.thread_id = entry->thread_id;
        native_stack.timestamp = timestamp_ns;
        
        if (state->config.native_unwinding) {
            RegisterState regs;
            if (get_register_state(entry->mach_port, &regs) == 0) {
                walk_stack(entry, &regs, &native_stack, state->config.max_stack_depth);
                MACH_DEBUG("thread %llu: captured %d native frames", 
                           (unsigned long long)entry->thread_id, native_stack.depth);
            } else {
                MACH_DEBUG("thread %llu: failed to get register state", 
                           (unsigned long long)entry->thread_id);
            }
        }
        
        /* ================================================================
         * PHASE 2: Capture Python Stack
         * ================================================================
         */
        uintptr_t python_frames[SPPROF_MAX_STACK_DEPTH];
        uintptr_t instr_ptrs[SPPROF_MAX_STACK_DEPTH];
        int python_depth = 0;
        
#ifdef SPPROF_USE_INTERNAL_API
        /* Capture Python frames from the suspended thread's state.
         * This is safe because the target thread is suspended. */
        python_depth = _spprof_capture_frames_with_instr_from_tstate(
            tstate,
            python_frames,
            instr_ptrs,
            state->config.max_stack_depth
        );
        
        MACH_DEBUG("thread %llu: captured %d Python frames", 
                   (unsigned long long)entry->thread_id, python_depth);
        
        /*
         * SAFETY: Add references to code objects via registry.
         *
         * This addresses the use-after-free issue where:
         * 1. We capture raw PyCodeObject* pointers here
         * 2. GC could run after we release GIL
         * 3. Code objects might be freed before resolver runs
         *
         * By INCREF'ing via the registry, we guarantee the code objects
         * remain valid until the resolver processes them.
         *
         * NOTE: This MUST happen:
         * - While holding the GIL (Py_INCREF requirement)
         * - While the thread is still suspended (to ensure pointer validity)
         */
        if (python_depth > 0) {
            uint64_t gc_epoch = code_registry_get_gc_epoch();
            code_registry_add_refs_batch(python_frames, python_depth, gc_epoch);
        }
#endif
        
        /* ================================================================
         * Resume thread IMMEDIATELY after capture and INCREF
         * ================================================================
         */
        kr = thread_resume(entry->mach_port);
        if (kr != KERN_SUCCESS) {
            MACH_DEBUG("thread %llu: resume failed with kr=%d", 
                       (unsigned long long)entry->thread_id, kr);
        }
        
        uint64_t suspend_end = mach_absolute_time();
        uint64_t suspend_ns = mach_to_ns(suspend_end - suspend_start);
        
        /* Update statistics (atomic operations for concurrent access) */
        atomic_fetch_add_explicit(&state->stats.suspend_time_ns, suspend_ns, memory_order_relaxed);
        
        /* Update max_suspend_ns - racy but acceptable for stats */
        uint64_t current_max = atomic_load_explicit(&state->stats.max_suspend_ns, memory_order_relaxed);
        if (suspend_ns > current_max) {
            /* Simple store is fine - worst case we miss an update, acceptable for stats */
            atomic_store_explicit(&state->stats.max_suspend_ns, suspend_ns, memory_order_relaxed);
        }
        
        atomic_fetch_add_explicit(&state->stats.threads_sampled, 1, memory_order_relaxed);
        
        /* ================================================================
         * Write mixed sample to ring buffer
         * ================================================================
         * Both Python frames and native PCs are stored. The resolver
         * will merge them using the "Trim & Sandwich" algorithm.
         *
         * Note: Ring buffer write is very fast (~1μs) and doesn't need
         * to be deferred outside the GIL section.
         */
        if (python_depth > 0 || native_stack.depth > 0) {
            if (write_mixed_sample_to_ringbuffer(
                    entry->thread_id, timestamp_ns,
                    python_frames, instr_ptrs, python_depth,
                    &native_stack,
                    state->ringbuffer)) {
                atomic_fetch_add_explicit(&state->stats.samples_captured, 1, memory_order_relaxed);
            } else {
                atomic_fetch_add_explicit(&state->stats.samples_dropped, 1, memory_order_relaxed);
            }
        }
    }
    
    PyGILState_Release(gstate);
}

/**
 * Legacy single-thread sample function (not used in current implementation).
 */
SPPROF_UNUSED
static int sample_thread(ThreadEntry* entry, MachSamplerState* state) {
    (void)entry;
    (void)state;
    /* This function is now deprecated - use sample_all_threads instead */
    return 0;
}

/*
 * =============================================================================
 * Sampler Thread (T025)
 * =============================================================================
 */

/**
 * Sampler thread main function.
 *
 * This thread wakes up at regular intervals and samples all registered threads.
 * It acquires the GIL briefly during each sample to safely access Python's
 * thread state list.
 */
static void* sampler_thread_func(void* arg) {
    MachSamplerState* state = (MachSamplerState*)arg;
    
    /* Store our own thread port (to skip during sampling) */
    state->sampler_mach_thread = mach_thread_self();
    state->registry.sampler_thread = state->sampler_mach_thread;
    
    MACH_DEBUG("sampler thread started, mach_port=%d", state->sampler_mach_thread);
    
    uint64_t next_time = mach_absolute_time() + state->interval_mach;
    uint64_t compact_counter = 0;
    
    while (atomic_load_explicit(&state->running, memory_order_acquire)) {
        /* Precise sleep until next sample time */
        mach_wait_until(next_time);
        
        if (!atomic_load_explicit(&state->running, memory_order_acquire)) {
            break;
        }
        
        /* Take snapshot of threads from our registry */
        ThreadSnapshot snapshot;
        registry_snapshot(&state->registry, &snapshot);
        
        MACH_DEBUG("sampling %zu threads", snapshot.count);
        
        /* Sample all threads (acquires GIL internally) */
        sample_all_threads(&snapshot, state);
        
        /* Schedule next sample */
        next_time += state->interval_mach;
        
        /* Catch up if we fell behind */
        uint64_t now = mach_absolute_time();
        if (next_time < now) {
            next_time = now + state->interval_mach;
        }
        
        /* Periodically compact the registry */
        compact_counter++;
        if (compact_counter >= 100) {
            registry_compact(&state->registry);
            compact_counter = 0;
        }
    }
    
    MACH_DEBUG("sampler thread exiting");
    return NULL;
}

/*
 * =============================================================================
 * Public API Implementation (T019-T020, T026-T027)
 * =============================================================================
 */

int mach_sampler_init(void) {
    if (atomic_load_explicit(&g_state.initialized, memory_order_acquire)) {
        MACH_DEBUG("init: already initialized");
        errno = EBUSY;
        return -1;
    }
    
    MACH_DEBUG("init: starting initialization");
    
    memset(&g_state, 0, sizeof(g_state));
    
    /* Initialize timing */
    mach_timebase_info(&g_state.timebase);
    MACH_DEBUG("init: timebase numer=%u denom=%u", 
               g_state.timebase.numer, g_state.timebase.denom);
    
    /* Initialize registry */
    if (registry_init(&g_state.registry) != 0) {
        MACH_DEBUG("init: registry_init failed");
        return -1;
    }
    
    /* Install pthread introspection hook */
    g_state.registry.prev_hook = pthread_introspection_hook_install(introspection_hook);
    g_state.registry.hook_installed = 1;
    MACH_DEBUG("init: introspection hook installed (prev=%p)", 
               (void*)g_state.registry.prev_hook);
    
    /* Register existing threads (main thread at minimum) */
    registry_add(&g_state.registry, pthread_self());
    
    /* Set default config */
    g_state.config.max_stack_depth = SPPROF_MAX_STACK_DEPTH;
    g_state.config.native_unwinding = 1;
    
    atomic_store_explicit(&g_state.initialized, 1, memory_order_release);
    
    MACH_DEBUG("init: complete");
    return 0;
}

void mach_sampler_cleanup(void) {
    if (!atomic_load_explicit(&g_state.initialized, memory_order_acquire)) {
        return;
    }
    
    /* Stop sampler if running */
    if (atomic_load_explicit(&g_state.running, memory_order_acquire)) {
        mach_sampler_stop();
    }
    
    /* Remove introspection hook */
    if (g_state.registry.hook_installed) {
        (void)pthread_introspection_hook_install(g_state.registry.prev_hook);
        g_state.registry.hook_installed = 0;
    }
    
    /* Cleanup registry */
    registry_cleanup(&g_state.registry);
    
    atomic_store_explicit(&g_state.initialized, 0, memory_order_release);
}

int mach_sampler_start(uint64_t interval_ns, RingBuffer* ringbuffer) {
    if (!atomic_load_explicit(&g_state.initialized, memory_order_acquire)) {
        MACH_DEBUG("start: not initialized");
        errno = EINVAL;
        return -1;
    }
    
    if (atomic_load_explicit(&g_state.running, memory_order_acquire)) {
        MACH_DEBUG("start: already running");
        errno = EAGAIN;
        return -1;
    }
    
    if (!ringbuffer) {
        MACH_DEBUG("start: NULL ringbuffer");
        errno = EINVAL;
        return -1;
    }
    
    /* Validate interval */
    if (interval_ns < MIN_INTERVAL_NS || interval_ns > MAX_INTERVAL_NS) {
        MACH_DEBUG("start: invalid interval %llu ns", 
                   (unsigned long long)interval_ns);
        errno = EINVAL;
        return -1;
    }
    
    MACH_DEBUG("start: interval=%llu ns (%llu Hz)", 
               (unsigned long long)interval_ns,
               (unsigned long long)(1000000000ULL / interval_ns));
    
    g_state.config.interval_ns = interval_ns;
    g_state.interval_mach = ns_to_mach(interval_ns);
    g_state.ringbuffer = ringbuffer;
    
    /* Reset statistics (use atomic stores for _Atomic fields) */
    atomic_store_explicit(&g_state.stats.samples_captured, 0, memory_order_relaxed);
    atomic_store_explicit(&g_state.stats.samples_dropped, 0, memory_order_relaxed);
    atomic_store_explicit(&g_state.stats.threads_sampled, 0, memory_order_relaxed);
    atomic_store_explicit(&g_state.stats.threads_skipped, 0, memory_order_relaxed);
    atomic_store_explicit(&g_state.stats.suspend_time_ns, 0, memory_order_relaxed);
    atomic_store_explicit(&g_state.stats.max_suspend_ns, 0, memory_order_relaxed);
    atomic_store_explicit(&g_state.stats.walk_errors, 0, memory_order_relaxed);
    
    atomic_store_explicit(&g_state.running, 1, memory_order_release);
    
    /* Create sampler thread */
    int ret = pthread_create(&g_state.sampler_pthread, NULL, 
                             sampler_thread_func, &g_state);
    if (ret != 0) {
        MACH_DEBUG("start: pthread_create failed with %d", ret);
        atomic_store_explicit(&g_state.running, 0, memory_order_release);
        errno = ENOMEM;
        return -1;
    }
    
    MACH_DEBUG("start: sampler thread created");
    return 0;
}

int mach_sampler_stop(void) {
    if (!atomic_load_explicit(&g_state.running, memory_order_acquire)) {
        MACH_DEBUG("stop: not running");
        errno = ESRCH;
        return -1;
    }
    
    MACH_DEBUG("stop: signaling sampler thread to stop");
    
    /* Signal stop */
    atomic_store_explicit(&g_state.running, 0, memory_order_release);
    
    /* Wait for sampler thread to exit */
    pthread_join(g_state.sampler_pthread, NULL);
    
    MACH_DEBUG("stop: complete - captured=%llu dropped=%llu sampled=%llu skipped=%llu",
               (unsigned long long)atomic_load_explicit(&g_state.stats.samples_captured, memory_order_relaxed),
               (unsigned long long)atomic_load_explicit(&g_state.stats.samples_dropped, memory_order_relaxed),
               (unsigned long long)atomic_load_explicit(&g_state.stats.threads_sampled, memory_order_relaxed),
               (unsigned long long)atomic_load_explicit(&g_state.stats.threads_skipped, memory_order_relaxed));
    
    return 0;
}

void mach_sampler_set_native_unwinding(int enabled) {
    g_state.config.native_unwinding = enabled ? 1 : 0;
}

int mach_sampler_get_native_unwinding(void) {
    return g_state.config.native_unwinding;
}

void mach_sampler_get_stats(uint64_t* samples_captured,
                            uint64_t* samples_dropped,
                            uint64_t* threads_sampled) {
    if (samples_captured) {
        *samples_captured = atomic_load_explicit(&g_state.stats.samples_captured, memory_order_relaxed);
    }
    if (samples_dropped) {
        *samples_dropped = atomic_load_explicit(&g_state.stats.samples_dropped, memory_order_relaxed);
    }
    if (threads_sampled) {
        *threads_sampled = atomic_load_explicit(&g_state.stats.threads_sampled, memory_order_relaxed);
    }
}

void mach_sampler_get_extended_stats(uint64_t* samples_captured,
                                     uint64_t* samples_dropped,
                                     uint64_t* threads_sampled,
                                     uint64_t* threads_skipped,
                                     uint64_t* total_suspend_ns,
                                     uint64_t* max_suspend_ns,
                                     uint64_t* walk_errors) {
    if (samples_captured) {
        *samples_captured = atomic_load_explicit(&g_state.stats.samples_captured, memory_order_relaxed);
    }
    if (samples_dropped) {
        *samples_dropped = atomic_load_explicit(&g_state.stats.samples_dropped, memory_order_relaxed);
    }
    if (threads_sampled) {
        *threads_sampled = atomic_load_explicit(&g_state.stats.threads_sampled, memory_order_relaxed);
    }
    if (threads_skipped) {
        *threads_skipped = atomic_load_explicit(&g_state.stats.threads_skipped, memory_order_relaxed);
    }
    if (total_suspend_ns) {
        *total_suspend_ns = atomic_load_explicit(&g_state.stats.suspend_time_ns, memory_order_relaxed);
    }
    if (max_suspend_ns) {
        *max_suspend_ns = atomic_load_explicit(&g_state.stats.max_suspend_ns, memory_order_relaxed);
    }
    if (walk_errors) {
        *walk_errors = atomic_load_explicit(&g_state.stats.walk_errors, memory_order_relaxed);
    }
}

size_t mach_sampler_thread_count(void) {
    size_t count;
    pthread_mutex_lock(&g_state.registry.lock);
    count = g_state.registry.count;
    pthread_mutex_unlock(&g_state.registry.lock);
    return count;
}

#endif /* __APPLE__ */

