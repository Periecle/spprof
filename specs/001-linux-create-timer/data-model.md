# Data Model: Linux timer_create Robustness Improvements

**Feature Branch**: `001-linux-create-timer`  
**Date**: 2025-12-01  
**Purpose**: Define data structures for the enhanced Linux timer implementation

---

## Entity Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                    Platform Layer (linux.c)                      │
├─────────────────────────────────────────────────────────────────┤
│  ┌─────────────────┐    ┌──────────────────────────────────┐   │
│  │  Global State   │    │      ThreadTimerRegistry         │   │
│  │ ─────────────── │    │ ────────────────────────────────│   │
│  │ main_timer      │    │  Hash Table (uthash)            │   │
│  │ interval_ns     │    │  ┌───────────────────────────┐  │   │
│  │ paused          │    │  │ ThreadTimerEntry (per TID)│  │   │
│  │ total_overruns  │    │  │ - tid (key)               │  │   │
│  │ create_failures │    │  │ - timer_id                │  │   │
│  └─────────────────┘    │  │ - overruns                │  │   │
│                         │  │ - active                  │  │   │
│  ┌─────────────────┐    │  └───────────────────────────┘  │   │
│  │  Thread-Local   │    │        ↑ (multiple entries)     │   │
│  │ ─────────────── │    └──────────────────────────────────┘   │
│  │ tl_timer_id     │                                           │
│  │ tl_timer_active │    ┌──────────────────────────────────┐   │
│  └─────────────────┘    │      TimerStatistics             │   │
│                         │ ────────────────────────────────│   │
│                         │ - samples_captured               │   │
│                         │ - samples_dropped                │   │
│                         │ - timer_overruns                 │   │
│                         │ - timer_create_failures          │   │
│                         │ - registered_threads             │   │
│                         └──────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
```

---

## 1. ThreadTimerEntry

**Purpose**: Represents a single thread's timer state in the registry.

### Structure Definition

```c
typedef struct ThreadTimerEntry {
    pid_t tid;              // Key: Linux thread ID (gettid())
    timer_t timer_id;       // POSIX timer handle
    uint64_t overruns;      // Accumulated timer overruns for this thread
    int active;             // 1 if timer is running, 0 if paused/stopped
    UT_hash_handle hh;      // uthash: makes structure hashable
} ThreadTimerEntry;
```

### Field Specifications

| Field | Type | Description | Constraints |
|-------|------|-------------|-------------|
| `tid` | `pid_t` | Linux thread ID | Unique within process, obtained via `gettid()` or `syscall(SYS_gettid)` |
| `timer_id` | `timer_t` | POSIX timer handle | Valid handle from `timer_create()`, or NULL if creation failed |
| `overruns` | `uint64_t` | Cumulative overrun count | Incremented from `timer_getoverrun()` results |
| `active` | `int` | Timer running state | 0 = paused/stopped, 1 = running |
| `hh` | `UT_hash_handle` | uthash internals | Managed by uthash macros |

### Lifecycle

```
                    ┌──────────────────┐
                    │    CREATED       │
       timer_create │ (malloc + init)  │
       ─────────────►                  │
                    └────────┬─────────┘
                             │
                    ┌────────▼─────────┐
                    │     ACTIVE       │
                    │ (timer running)  │◄────────┐
                    └────────┬─────────┘         │
                             │                   │
              pause          │         resume    │
              ───────────────┼───────────────────┘
                             │
                    ┌────────▼─────────┐
                    │     PAUSED       │
                    │ (timer disarmed) │
                    └────────┬─────────┘
                             │
              unregister     │
              ───────────────┼──────────────────
                             │
                    ┌────────▼─────────┐
                    │    DELETED       │
                    │ (timer_delete +  │
                    │  free memory)    │
                    └──────────────────┘
```

---

## 2. ThreadTimerRegistry (Conceptual)

**Purpose**: Hash table managing all thread timer entries.

### Structure (Implicit via uthash)

```c
// Registry is simply a pointer to the first entry
// uthash manages the hash table internally
static ThreadTimerEntry* g_thread_registry = NULL;

// Protected by RWLock for thread safety
static pthread_rwlock_t g_registry_lock = PTHREAD_RWLOCK_INITIALIZER;
```

### Operations

| Operation | Lock Type | Time Complexity | Description |
|-----------|-----------|-----------------|-------------|
| `registry_add_thread` | Write | O(1) avg | Add new thread entry |
| `registry_find_thread` | Read | O(1) avg | Find entry by TID |
| `registry_remove_thread` | Write | O(1) avg | Remove and free entry |
| `registry_count` | Read | O(1) | Get number of entries |
| `registry_iterate` | Read | O(n) | Iterate all entries |
| `registry_cleanup_all` | Write | O(n) | Delete all entries |

### Thread Safety Guarantees

| Scenario | Thread A | Thread B | Safe? |
|----------|----------|----------|-------|
| Add + Add | Write | Write | ✅ (serialized) |
| Add + Find | Write | Read | ✅ (readers wait) |
| Find + Find | Read | Read | ✅ (concurrent) |
| Remove + Find | Write | Read | ✅ (readers wait) |
| Iterate + Add | Read | Write | ✅ (writer waits) |

---

## 3. Global State

**Purpose**: Module-level state for the Linux platform implementation.

### Structure Definition

```c
// Timer state
static timer_t g_main_timer = NULL;          // Main thread's timer
static uint64_t g_interval_ns = 0;           // Configured sampling interval
static int g_platform_initialized = 0;       // Init flag
static int g_paused = 0;                     // Pause state

// Statistics (atomic for signal-safe reads)
static _Atomic uint64_t g_total_overruns = 0;      // Sum of all timer overruns
static _Atomic uint64_t g_timer_create_failures = 0; // Failed timer_create calls
```

### Thread-Local State

```c
// Fast path: avoid registry lookup for common operations
static __thread timer_t tl_timer_id = NULL;
static __thread int tl_timer_active = 0;
```

---

## 4. TimerStatistics

**Purpose**: Aggregated statistics exposed through the platform API.

### Structure Definition

```c
typedef struct {
    uint64_t samples_captured;      // From signal_handler
    uint64_t samples_dropped;       // From signal_handler
    uint64_t timer_overruns;        // From g_total_overruns
    uint64_t timer_create_failures; // From g_timer_create_failures
    uint64_t registered_threads;    // Count from registry
    uint64_t active_threads;        // Count of active=1 entries
} TimerStatistics;
```

### Collection

```c
void platform_get_extended_stats(TimerStatistics* stats) {
    // From signal handler (existing)
    stats->samples_captured = signal_handler_samples_captured();
    stats->samples_dropped = signal_handler_samples_dropped();
    
    // From platform layer (new)
    stats->timer_overruns = atomic_load(&g_total_overruns);
    stats->timer_create_failures = atomic_load(&g_timer_create_failures);
    
    // From registry (new)
    pthread_rwlock_rdlock(&g_registry_lock);
    stats->registered_threads = HASH_COUNT(g_thread_registry);
    stats->active_threads = 0;
    ThreadTimerEntry *entry, *tmp;
    HASH_ITER(hh, g_thread_registry, entry, tmp) {
        if (entry->active) {
            stats->active_threads++;
        }
    }
    pthread_rwlock_unlock(&g_registry_lock);
}
```

---

## 5. Memory Layout

### ThreadTimerEntry Memory

```
+------------------+
| ThreadTimerEntry |  ~80-100 bytes (depends on uthash internals)
+------------------+
| tid      (4B)    |
| timer_id (8B)    |
| overruns (8B)    |
| active   (4B)    |
| padding  (~4B)   |
| hh (uthash ~48B) |
+------------------+
```

### Memory Usage Estimation

| Threads | Memory (entries) | Hash Overhead | Total |
|---------|------------------|---------------|-------|
| 100 | ~10 KB | ~1 KB | ~11 KB |
| 500 | ~50 KB | ~5 KB | ~55 KB |
| 1000 | ~100 KB | ~10 KB | ~110 KB |
| 5000 | ~500 KB | ~50 KB | ~550 KB |

**Note**: uthash automatically resizes the hash table. Default load factor is 0.75.

---

## 6. Validation Rules

### ThreadTimerEntry Invariants

1. **TID uniqueness**: Each `tid` appears at most once in registry
2. **Timer validity**: If `timer_id != NULL`, it must be a valid timer handle
3. **Active consistency**: If `active == 1`, timer must be armed (non-zero interval)
4. **Overrun monotonicity**: `overruns` only increases (never decremented)

### Registry Invariants

1. **Lock discipline**: All registry access must be within lock scope
2. **Memory ownership**: Registry owns all `ThreadTimerEntry` memory
3. **Cleanup completeness**: `registry_cleanup_all` must free all entries

### State Transition Rules

| From State | To State | Trigger | Validation |
|------------|----------|---------|------------|
| - | ACTIVE | `platform_register_thread` | `timer_create` succeeded |
| ACTIVE | PAUSED | `platform_timer_pause` | `timer_settime` with zero interval |
| PAUSED | ACTIVE | `platform_timer_resume` | `timer_settime` with saved interval |
| ACTIVE | DELETED | `platform_unregister_thread` | `timer_delete` called |
| PAUSED | DELETED | `platform_cleanup` | `timer_delete` called |

