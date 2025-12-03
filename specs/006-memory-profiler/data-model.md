# Data Model: Memory Allocation Profiler

**Feature**: 006-memory-profiler  
**Date**: December 3, 2024

---

## Overview

This document defines the core data structures for the memory profiler. The design prioritizes:
1. **Lock-free operations**: Hot path must avoid locks
2. **Memory efficiency**: Bounded footprint regardless of profiling duration
3. **Atomic consistency**: No torn reads during concurrent snapshot

---

## Entity Relationship Diagram

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              MemProfGlobalState                              │
│  (Singleton - immutable after init except atomic flags)                     │
├─────────────────────────────────────────────────────────────────────────────┤
│  sampling_rate: uint64                                                       │
│  active_alloc: atomic<int>                                                   │
│  active_free: atomic<int>                                                    │
│  initialized: atomic<int>                                                    │
└─────────────────────────────────────────────────────────────────────────────┘
         │                              │                              │
         │ owns                         │ owns                         │ owns
         ▼                              ▼                              ▼
┌─────────────────────┐    ┌─────────────────────┐    ┌─────────────────────┐
│    HeapMap          │    │   StackTable        │    │   BloomFilter       │
│    (1M entries)     │    │   (4K-64K entries)  │    │   (128KB)           │
├─────────────────────┤    ├─────────────────────┤    ├─────────────────────┤
│  HeapMapEntry[]     │    │  StackEntry[]       │    │  uint8_t[]          │
└─────────────────────┘    └─────────────────────┘    └─────────────────────┘
         │                              │
         │ contains                     │ contains
         ▼                              ▼
┌─────────────────────┐    ┌─────────────────────┐
│   HeapMapEntry      │───▶│    StackEntry       │
│   (24 bytes)        │    │   (~544 bytes)      │
├─────────────────────┤    ├─────────────────────┤
│  ptr: atomic<uptr>  │    │  hash: atomic<u64>  │
│  metadata: atomic64 │    │  depth: u16         │
│  birth_seq: atomic64│    │  flags: u16         │
│  timestamp: u64     │    │  frames: uintptr[]  │
└─────────────────────┘    └─────────────────────┘

                    ┌─────────────────────────────┐
                    │   MemProfThreadState        │
                    │   (Per-thread TLS)          │
                    ├─────────────────────────────┤
                    │  byte_counter: int64        │
                    │  prng_state: uint64[2]      │
                    │  inside_profiler: int       │
                    │  frame_buffer: uintptr[]    │
                    └─────────────────────────────┘
```

---

## C Data Structures

### HeapMapEntry (24 bytes)

```c
/**
 * HeapMapEntry - Single entry in the live heap map
 *
 * State machine for `ptr` field:
 *   0         = EMPTY (slot available)
 *   1         = RESERVED (insert in progress)
 *   ~0ULL     = TOMBSTONE (freed, slot reusable)
 *   valid ptr = OCCUPIED (allocation tracked)
 */
typedef struct {
    _Atomic uintptr_t ptr;        /* Key: allocated pointer */
    _Atomic uint64_t  metadata;   /* Packed: stack_id | size | weight */
    _Atomic uint64_t  birth_seq;  /* Sequence number at allocation time */
    uint64_t          timestamp;  /* Wall clock time (for duration reporting) */
} HeapMapEntry;

/* Packed metadata format: stack_id (20 bits) | size (24 bits) | weight (20 bits) */
#define METADATA_PACK(stack_id, size, weight) \
    ((((uint64_t)(stack_id) & 0xFFFFF) << 44) | \
     (((uint64_t)(size) & 0xFFFFFF) << 20) | \
     ((uint64_t)(weight) & 0xFFFFF))

#define METADATA_STACK_ID(m) (((m) >> 44) & 0xFFFFF)
#define METADATA_SIZE(m)     (((m) >> 20) & 0xFFFFFF)
#define METADATA_WEIGHT(m)   ((m) & 0xFFFFF)

/* State constants */
#define HEAP_ENTRY_EMPTY     ((uintptr_t)0)
#define HEAP_ENTRY_RESERVED  ((uintptr_t)1)
#define HEAP_ENTRY_TOMBSTONE (~(uintptr_t)0)
```

**Field Descriptions**:

| Field | Type | Description |
|-------|------|-------------|
| `ptr` | atomic uintptr_t | Hash key; also encodes state (EMPTY/RESERVED/TOMBSTONE/valid) |
| `metadata` | atomic uint64 | Packed: stack_id, allocation size, sampling weight |
| `birth_seq` | atomic uint64 | Global sequence number when allocated (for ABA detection) |
| `timestamp` | uint64 | Monotonic timestamp in nanoseconds |

**Constraints**:
- `stack_id` ≤ 1,048,575 (20 bits)
- `size` ≤ 16,777,215 (24 bits, ~16 MB - larger allocations clamped)
- `weight` ≤ 1,048,575 (20 bits)

---

### StackEntry (~544 bytes)

```c
/**
 * StackEntry - Interned call stack
 *
 * Many allocations share the same call site. Interning saves memory
 * and enables O(1) stack comparison via stack_id.
 */
typedef struct {
    _Atomic uint64_t hash;        /* FNV-1a hash for lookup */
    uint16_t depth;               /* Number of valid frames */
    uint16_t flags;               /* RESOLVED, PYTHON_ATTRIBUTED, etc. */
    uintptr_t frames[64];         /* Raw return addresses (MEMPROF_MAX_STACK_DEPTH) */
    
    /* Resolved symbols (lazily populated) */
    char** function_names;        /* Array of function name strings */
    char** file_names;            /* Array of file name strings */
    int*   line_numbers;          /* Array of line numbers */
} StackEntry;

#define STACK_FLAG_RESOLVED        0x0001
#define STACK_FLAG_PYTHON_ATTR     0x0002
#define STACK_FLAG_TRUNCATED       0x0004
```

**Field Descriptions**:

| Field | Type | Description |
|-------|------|-------------|
| `hash` | atomic uint64 | FNV-1a hash for deduplication; 0 = empty slot |
| `depth` | uint16 | Number of valid frames in array |
| `flags` | uint16 | Status flags (resolved, truncated, etc.) |
| `frames` | uintptr_t[64] | Raw program counter addresses |
| `function_names` | char** | Resolved function names (lazy) |
| `file_names` | char** | Resolved file paths (lazy) |
| `line_numbers` | int* | Resolved line numbers (lazy) |

---

### MemProfThreadState (TLS, ~1 KB)

```c
/**
 * MemProfThreadState - Per-thread sampling state
 *
 * This is the ONLY mutable state accessed in the hot path.
 * All fields are thread-local, no synchronization needed.
 */
typedef struct {
    /* Sampling state */
    int64_t  byte_counter;        /* Countdown to next sample (signed!) */
    uint64_t prng_state[2];       /* xorshift128+ PRNG state */
    
    /* Safety */
    int      inside_profiler;     /* Re-entrancy guard */
    int      initialized;         /* TLS initialized flag */
    
    /* Pre-allocated sample buffer */
    uintptr_t frame_buffer[64];   /* MEMPROF_MAX_STACK_DEPTH */
    int       frame_depth;
    
    /* Per-thread statistics */
    uint64_t total_allocs;
    uint64_t total_frees;
    uint64_t sampled_allocs;
    uint64_t sampled_bytes;
    uint64_t skipped_reentrant;
} MemProfThreadState;
```

---

### MemProfGlobalState (Singleton)

```c
/**
 * MemProfGlobalState - Singleton profiler state
 */
typedef struct {
    /* Configuration (immutable after init) */
    uint64_t sampling_rate;
    int      capture_python;
    int      resolve_on_stop;
    
    /* State (atomic) */
    _Atomic int active_alloc;     /* Track new allocations */
    _Atomic int active_free;      /* Track frees */
    _Atomic int initialized;
    
    /* Data structures (mmap'd) */
    HeapMapEntry* heap_map;
    StackEntry*   stack_table;
    _Atomic uint32_t stack_count;
    
    /* Bloom filter */
    _Atomic(_Atomic uint8_t*) bloom_filter_ptr;
    _Atomic uint64_t bloom_ones_count;
    _Atomic int bloom_rebuild_in_progress;
    
    /* Global statistics (atomic) */
    _Atomic uint64_t total_samples;
    _Atomic uint64_t total_frees_tracked;
    _Atomic uint64_t heap_map_collisions;
    _Atomic uint64_t heap_map_insertions;
    _Atomic uint64_t heap_map_deletions;
    _Atomic uint64_t heap_map_full_drops;
    _Atomic uint64_t stack_table_collisions;
    _Atomic uint64_t bloom_rebuilds;
    _Atomic uint64_t death_during_birth;
    _Atomic uint64_t zombie_races_detected;
    _Atomic uint64_t tombstones_recycled;
    _Atomic uint64_t shallow_stack_warnings;
    
    /* Platform-specific state */
    void* platform_state;
} MemProfGlobalState;
```

---

## Python Data Classes

### AllocationSample

```python
@dataclass
class AllocationSample:
    """A single sampled allocation."""
    address: int              # Pointer address
    size: int                 # Actual allocation size (bytes)
    weight: int               # Sampling weight (= sampling_rate)
    estimated_bytes: int      # size × weight (contribution to estimate)
    timestamp_ns: int         # When allocated
    lifetime_ns: Optional[int] # Duration if freed, None if live
    stack: List[StackFrame]   # Call stack at allocation
    gc_epoch: int             # GC cycle when allocated (optional)
```

### StackFrame

```python
@dataclass
class StackFrame:
    """A frame in the allocation call stack."""
    address: int              # Raw program counter
    function: str             # Resolved function name
    file: str                 # Source file path
    line: int                 # Line number
    is_python: bool           # True if Python frame, False if native
```

### HeapSnapshot

```python
@dataclass
class HeapSnapshot:
    """Snapshot of live (unfreed) sampled allocations."""
    samples: List[AllocationSample]
    total_samples: int
    live_samples: int
    estimated_heap_bytes: int
    timestamp_ns: int
    frame_pointer_health: FramePointerHealth
    
    def top_allocators(self, n: int = 10) -> List[Dict]:
        """Get top N allocation sites by estimated bytes."""
        ...
    
    def save(self, path: Path, format: str = "speedscope") -> None:
        """Save snapshot to file."""
        ...
```

### FramePointerHealth

```python
@dataclass
class FramePointerHealth:
    """Metrics for native stack capture quality."""
    shallow_stack_warnings: int
    total_native_stacks: int
    avg_native_depth: float
    min_native_depth: int
    truncation_rate: float       # shallow_warnings / total
    
    @property
    def confidence(self) -> str:
        """'high' (<5%), 'medium' (5-20%), 'low' (>20%)"""
        ...
```

### MemProfStats

```python
@dataclass
class MemProfStats:
    """Profiler statistics."""
    total_samples: int
    live_samples: int
    freed_samples: int
    unique_stacks: int
    estimated_heap_bytes: int
    heap_map_load_percent: float
    collisions: int
    sampling_rate_bytes: int
```

---

## State Transitions

### HeapMapEntry State Machine

```
              malloc                    malloc
    ┌─────────────────────┐   ┌─────────────────────┐
    │                     │   │                     │
    ▼                     │   ▼                     │
 EMPTY ──────────────────►│ RESERVED ─────────────►│ ptr (OCCUPIED)
    ▲                     │     │                   │
    │                     │     │ free              │ free
    │   compaction        │     │ (death during     │ (normal)
    │                     │     │  birth)           │
    └─────────────────────┴─────┴───────────────────┘
                                       │
                                       ▼
                                   TOMBSTONE
                                       │
              ┌────────────────────────┴────────────────────────┐
              │                                                  │
              ▼ malloc (recycle)                                ▼ compaction
           RESERVED                                           EMPTY
```

### Profiler Lifecycle States

```
UNINITIALIZED ──────[init()]──────► INITIALIZED
       │                                  │
       │                            [start()]
       │                                  │
       │                                  ▼
       │                               ACTIVE
       │                                  │
       │                            [stop()]
       │                                  │
       │                                  ▼
       │                              STOPPED ◄──── [start()]
       │                                  │
       │                           [shutdown()]
       │                                  │
       └──────────────────────────────────┴──────► TERMINATED
```

---

## Capacity Limits

| Structure | Capacity | Memory | Notes |
|-----------|----------|--------|-------|
| HeapMap | 1,048,576 entries | 24 MB | Fixed at init |
| StackTable | 4,096 - 65,536 entries | 2-35 MB | Dynamic growth |
| BloomFilter | 1,048,576 bits | 128 KB | Fixed |
| TLS per thread | 1 structure | ~1 KB | Auto-allocated |
| **Total** | - | **27-60 MB** | - |

---

## Validation Rules

### HeapMapEntry
- `ptr` must transition through valid state machine
- `metadata` only written after CAS claims slot
- `birth_seq` must be monotonically increasing

### StackEntry
- `hash` = 0 indicates empty slot
- `depth` must be ≤ MEMPROF_MAX_STACK_DEPTH (64)
- `frames[0..depth-1]` must be valid pointers

### AllocationSample
- `size` must be > 0
- `weight` must be > 0
- `estimated_bytes` = `weight` (not `size × weight` - weight IS the estimate contribution)
- `lifetime_ns` is None for live allocations, positive for freed

### HeapSnapshot
- `live_samples` ≤ `total_samples`
- `estimated_heap_bytes` = Σ(sample.weight) for live samples
- `frame_pointer_health.truncation_rate` = shallow_warnings / total_stacks

