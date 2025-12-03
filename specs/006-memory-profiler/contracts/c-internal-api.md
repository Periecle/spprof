# C Internal API Contract: Memory Profiler

**Feature**: 006-memory-profiler  
**Date**: December 3, 2024

---

## Overview

This document defines the internal C API for the memory profiler subsystem. These functions are NOT exposed to Python directly; they are called by the platform interposition layer and Python bindings.

---

## Core Lifecycle API

### `memprof_init`

```c
/**
 * Initialize the memory profiler.
 *
 * Allocates data structures (heap map, stack table, bloom filter) using mmap.
 * Must be called before start().
 *
 * Thread safety: NOT thread-safe. Call once from main thread.
 *
 * @param sampling_rate  Average bytes between samples (default: 512 * 1024)
 * @return 0 on success, -1 on error (sets errno)
 */
int memprof_init(uint64_t sampling_rate);
```

---

### `memprof_start`

```c
/**
 * Start memory profiling.
 *
 * Installs platform-specific interposition hooks.
 * Sets active_alloc and active_free flags to 1.
 *
 * Thread safety: Thread-safe. Can be called from any thread.
 *
 * @return 0 on success, -1 if already running or not initialized
 */
int memprof_start(void);
```

---

### `memprof_stop`

```c
/**
 * Stop memory profiling (new allocations only).
 *
 * Sets active_alloc to 0 but keeps active_free at 1.
 * This ensures allocations made during profiling are correctly marked
 * as freed if they're deallocated after stop() is called.
 *
 * Thread safety: Thread-safe.
 *
 * @return 0 on success, -1 if not running
 */
int memprof_stop(void);
```

---

### `memprof_shutdown`

```c
/**
 * Shutdown profiler completely.
 *
 * ⚠️ ONE-WAY DOOR: Cannot restart after shutdown.
 *
 * - Disables all hooks (active_alloc = active_free = 0)
 * - Cleans up leaked Bloom filters
 * - Does NOT munmap heap_map/stack_table (safety: in-flight hooks)
 *
 * Thread safety: Call once from main thread at exit.
 */
void memprof_shutdown(void);
```

---

## Snapshot API

### `memprof_get_snapshot`

```c
/**
 * Get snapshot of live allocations.
 *
 * Allocates output array using malloc - caller must call memprof_free_snapshot().
 * Iterates heap map with acquire loads for consistency.
 *
 * @param out_entries  Output: array of HeapMapEntry copies
 * @param out_count    Output: number of entries
 * @return 0 on success, -1 on error
 */
int memprof_get_snapshot(HeapMapEntry** out_entries, size_t* out_count);
```

---

### `memprof_free_snapshot`

```c
/**
 * Free a snapshot returned by memprof_get_snapshot().
 */
void memprof_free_snapshot(HeapMapEntry* entries);
```

---

### `memprof_get_stats`

```c
/**
 * Get profiler statistics.
 *
 * Thread-safe: Uses atomic loads.
 *
 * @param out  Output statistics structure
 * @return 0 on success
 */
int memprof_get_stats(MemProfStats* out);
```

---

### `memprof_resolve_symbols`

```c
/**
 * Resolve symbols for all captured stacks.
 *
 * Uses dladdr/DbgHelp for native symbols.
 * NOT async-signal-safe - call from safe context only.
 *
 * Thread safety: NOT thread-safe. Call from single thread.
 *
 * @return Number of stacks resolved
 */
int memprof_resolve_symbols(void);
```

---

## Heap Map API

### `heap_map_init`

```c
/**
 * Initialize the heap map.
 *
 * Uses mmap to allocate backing array (avoids malloc recursion).
 * Capacity: MEMPROF_HEAP_MAP_CAPACITY (1M entries, ~24 MB)
 *
 * @return 0 on success, -1 on error
 */
int heap_map_init(void);
```

---

### `heap_map_reserve`

```c
/**
 * Reserve a slot for a sampled allocation (Phase 1 of insert).
 *
 * Uses CAS to claim EMPTY or TOMBSTONE slot as RESERVED.
 * Stores ptr in metadata temporarily for matching during "death during birth".
 *
 * Lock-free: Uses CAS on ptr field.
 *
 * @param ptr  Allocated pointer address
 * @return Slot index on success, -1 if table full
 */
int heap_map_reserve(uintptr_t ptr);
```

---

### `heap_map_finalize`

```c
/**
 * Finalize a reserved slot with metadata (Phase 2 of insert).
 *
 * CAS: RESERVED → ptr. If fails, "death during birth" occurred.
 *
 * @param slot_idx        Slot index from heap_map_reserve()
 * @param ptr             Allocated pointer
 * @param packed_metadata Packed stack_id, size, weight
 * @return 1 on success, 0 if "death during birth"
 */
int heap_map_finalize(int slot_idx, uintptr_t ptr, uint64_t packed_metadata);
```

---

### `heap_map_remove`

```c
/**
 * Remove a freed allocation from heap map.
 *
 * Handles both OCCUPIED → TOMBSTONE and RESERVED → TOMBSTONE transitions.
 * Uses sequence number to detect macOS ABA race.
 *
 * Lock-free: Never spins, never blocks.
 *
 * @param ptr           Freed pointer address
 * @param free_seq      Sequence number captured at free() entry
 * @param free_timestamp Timestamp for duration calculation
 * @param out_stack_id  Output: stack ID of removed entry
 * @param out_size      Output: size of removed entry
 * @param out_weight    Output: weight of removed entry
 * @param out_duration  Output: lifetime in nanoseconds
 * @return 1 if found and removed, 0 if not found
 */
int heap_map_remove(uintptr_t ptr, uint64_t free_seq, uint64_t free_timestamp,
                    uint32_t* out_stack_id, uint32_t* out_size,
                    uint32_t* out_weight, uint64_t* out_duration);
```

---

### `heap_map_load_percent`

```c
/**
 * Get current load factor.
 *
 * @return Load factor as percentage (0-100)
 */
int heap_map_load_percent(void);
```

---

## Stack Intern API

### `stack_table_init`

```c
/**
 * Initialize the stack intern table.
 *
 * Initial capacity: MEMPROF_STACK_TABLE_INITIAL (4K entries)
 * Maximum capacity: MEMPROF_STACK_TABLE_MAX (64K default, configurable)
 *
 * @return 0 on success, -1 on error
 */
int stack_table_init(void);
```

---

### `stack_table_intern`

```c
/**
 * Intern a stack trace, returning a unique 32-bit ID.
 *
 * Lock-free: Uses CAS on hash field.
 * May insert duplicate if two threads race (harmless).
 *
 * @param frames  Array of return addresses
 * @param depth   Number of frames
 * @param hash    Pre-computed FNV-1a hash
 * @return Stack ID (index), or UINT32_MAX if full
 */
uint32_t stack_table_intern(const uintptr_t* frames, int depth, uint64_t hash);
```

---

### `stack_table_get`

```c
/**
 * Get a stack entry by ID.
 *
 * @param stack_id  Stack ID from stack_table_intern()
 * @return Pointer to StackEntry, or NULL if invalid
 */
const StackEntry* stack_table_get(uint32_t stack_id);
```

---

## Bloom Filter API

### `bloom_add`

```c
/**
 * Add pointer to Bloom filter.
 *
 * Uses atomic OR for thread safety.
 * Access via g_memprof.bloom_filter_ptr for atomic swap support.
 *
 * @param ptr  Pointer to add
 */
void bloom_add(uintptr_t ptr);
```

---

### `bloom_might_contain`

```c
/**
 * Check if pointer MIGHT be in set.
 *
 * @param ptr  Pointer to check
 * @return 0 = definitely NOT sampled, 1 = maybe sampled
 */
int bloom_might_contain(uintptr_t ptr);
```

---

### `bloom_rebuild_from_heap`

```c
/**
 * Rebuild Bloom filter from live heap map (background task).
 *
 * Called when saturation exceeds threshold.
 * Intentionally leaks old filter (safety over cleanup).
 *
 * @return 0 on success, -1 on error
 */
int bloom_rebuild_from_heap(void);
```

---

## Sampling Engine API

### `capture_native_stack`

```c
/**
 * Capture native stack frames via frame pointer walking.
 *
 * CRITICAL: Must NOT call malloc or any function that might.
 * Uses only stack-allocated data and direct memory reads.
 *
 * @param frames     Output array for return addresses
 * @param max_depth  Maximum frames to capture
 * @param skip       Frames to skip (exclude profiler frames)
 * @return Number of frames captured
 */
int capture_native_stack(uintptr_t* frames, int max_depth, int skip);
```

---

### `capture_mixed_stack`

```c
/**
 * Capture both Python and native frames.
 *
 * Uses framewalker.c for Python frames.
 * Merges results using "Trim & Sandwich" algorithm.
 *
 * @param out  Output structure with native and Python frames
 * @return Total frame count
 */
int capture_mixed_stack(MixedStackCapture* out);
```

---

### `next_sample_threshold`

```c
/**
 * Generate next sampling threshold using exponential distribution.
 *
 * Uses xorshift128+ PRNG for speed.
 * Result: -mean × ln(U) where U ~ Uniform(0,1)
 *
 * @param mean_bytes  Average bytes between samples
 * @return Threshold in bytes (always positive)
 */
int64_t next_sample_threshold(uint64_t mean_bytes);
```

---

## Platform Interposition API

### `memprof_linux_install`

```c
/**
 * Install Linux LD_PRELOAD hooks.
 *
 * Resolves real malloc/free via dlsym(RTLD_NEXT, ...).
 * Handles bootstrap heap for init-time allocations.
 *
 * @return 0 on success, -1 on error
 */
int memprof_linux_install(void);
```

---

### `memprof_darwin_install`

```c
/**
 * Install macOS malloc_logger callback.
 *
 * Uses atomic flag for thread-safe installation.
 *
 * @return 0 on success, -1 if already installed
 */
int memprof_darwin_install(void);
```

---

### `memprof_darwin_remove`

```c
/**
 * Remove macOS malloc_logger callback.
 *
 * Brief delay to let in-flight callbacks complete.
 */
void memprof_darwin_remove(void);
```

---

## Thread Safety Summary

| Function | Thread Safety | Notes |
|----------|---------------|-------|
| `memprof_init` | NOT safe | Call once from main thread |
| `memprof_start` | Safe | Atomic flag transition |
| `memprof_stop` | Safe | Atomic flag transition |
| `memprof_shutdown` | NOT safe | Call at exit only |
| `memprof_get_snapshot` | Safe | Acquire loads |
| `memprof_get_stats` | Safe | Atomic loads |
| `heap_map_reserve` | Safe | Lock-free CAS |
| `heap_map_finalize` | Safe | Lock-free CAS |
| `heap_map_remove` | Safe | Lock-free |
| `stack_table_intern` | Safe | Lock-free CAS |
| `bloom_add` | Safe | Atomic OR |
| `bloom_might_contain` | Safe | Relaxed loads |

---

## Memory Ordering Requirements

| Operation | Ordering | Rationale |
|-----------|----------|-----------|
| `heap_map_reserve` CAS | acq_rel | Synchronize slot ownership |
| `heap_map_finalize` metadata store | relaxed | ptr publish provides sync |
| `heap_map_finalize` ptr CAS | release | Publish entry to readers |
| `heap_map_remove` ptr load | acquire | See latest metadata |
| `bloom_filter_ptr` store | release | Synchronize filter contents |
| `bloom_filter_ptr` load | acquire | See latest filter |
| Statistics counters | relaxed | Approximate counts OK |

---

## Error Codes

| Code | Meaning |
|------|---------|
| 0 | Success |
| -1 | General error (check errno) |
| UINT32_MAX | Stack table full (stack_table_intern) |

---

## Constants

```c
#define MEMPROF_MAX_STACK_DEPTH 64
#define MEMPROF_HEAP_MAP_CAPACITY (1 << 20)  /* 1M entries */
#define MEMPROF_HEAP_MAP_MASK (MEMPROF_HEAP_MAP_CAPACITY - 1)
#define MEMPROF_STACK_TABLE_INITIAL (1 << 12)  /* 4K entries */
#define MEMPROF_STACK_TABLE_MAX_DEFAULT (1 << 16)  /* 64K entries */
#define MEMPROF_MAX_PROBE 128
#define MEMPROF_DEFAULT_SAMPLING_RATE (512 * 1024)  /* 512 KB */
#define BLOOM_SIZE_BITS (1 << 20)  /* 1M bits */
#define BLOOM_SIZE_BYTES (BLOOM_SIZE_BITS / 8)  /* 128 KB */
#define BLOOM_HASH_COUNT 4
```

