# Research: Linux Free-Threading Support

**Feature**: 005-linux-freethreading  
**Date**: December 2, 2024

## 1. Speculative Reading Safety Model

### Decision: Use speculative frame reads with multi-layer validation

### Rationale

On x86-64 and ARM64, aligned pointer reads/writes are atomic at the hardware level. The danger in free-threaded Python isn't corrupted reads—it's reading stale or freed memory. The race window during frame chain updates is ~10-50 nanoseconds, while the sampling interval is 10ms. This yields a ~0.0005% chance of hitting the race window per sample.

**Key insight**: We don't need perfect synchronization—we need to detect and discard the rare corrupted samples.

### Alternatives Considered

| Approach | Overhead | Pros | Cons |
|----------|----------|------|------|
| **Speculative + Validation** | ~500ns | No coordination, no bias, no permissions | ~0.1% dropped samples |
| Futex Rendezvous | ~10-50μs | Guaranteed consistency | 20-100x slower, complex |
| PTRACE | ~100μs | Full control | Requires CAP_SYS_PTRACE |
| eBPF | ~1μs | Kernel-assisted | Requires CAP_BPF, complex |
| Py_AddPendingCall | ~1μs | Python-sanctioned | Heavy safepoint bias |

**Speculative reading is optimal** because it requires no thread coordination, has no safepoint bias, needs no special permissions, and is 20-100x faster than alternatives.

---

## 2. Memory Model Considerations

### Decision: Use acquire barriers on ARM64; plain loads on x86-64

### Rationale

**x86-64**: Strong memory model. All loads have implicit acquire semantics. Pointer reads are naturally ordered—if we see a new `frame->previous` value, all prior writes by that thread are visible.

**ARM64**: Weak memory model. Stores can be reordered. Without barriers, we might see a new pointer value but old data at that address. Solution: Use `__atomic_load_n(ptr, __ATOMIC_ACQUIRE)` for frame pointer reads.

### Implementation Pattern

```c
#if defined(__aarch64__)
    #define SPPROF_ATOMIC_LOAD_PTR(ptr) \
        __atomic_load_n((void**)(ptr), __ATOMIC_ACQUIRE)
#else
    /* x86-64: plain load is sufficient */
    #define SPPROF_ATOMIC_LOAD_PTR(ptr) (*(void**)(ptr))
#endif
```

### Alternatives Considered

- **Full sequential consistency (`__ATOMIC_SEQ_CST`)**: Unnecessary overhead; acquire is sufficient for our read-only access pattern
- **Relaxed loads everywhere**: Unsafe on ARM64; could read stale data
- **Platform-specific assembly**: Harder to maintain; compiler intrinsics are portable and well-optimized

---

## 3. Pointer Validation Strategy

### Decision: Three-tier validation with early bail

### Rationale

Validation must be fast (in signal handler) and catch common corruption patterns:

1. **Heap bounds check**: Pointer within valid user-space range (0x10000 to 0x7FFFFFFFFFFF on x86-64)
2. **Alignment check**: Pointer 8-byte aligned (all Python objects are)
3. **Type check**: `obj->ob_type == cached_PyCode_Type`

If any check fails, we bail immediately—no crash, just a dropped sample.

### Validation Tiers

| Check | Catches | Cost |
|-------|---------|------|
| NULL check | Uninitialized pointers | 1 comparison |
| Bounds check | Wild pointers, freed addresses | 2 comparisons |
| Alignment check | Partially updated pointers | 1 AND + comparison |
| Type check | Type confusion, non-code objects | 1 pointer read + comparison |

### Alternatives Considered

- **mprotect-based validation**: Too expensive; system call overhead
- **/proc/self/maps parsing**: Not async-signal-safe
- **Checksum/magic numbers**: Requires Python internals modification

---

## 4. Cycle Detection

### Decision: Rolling window of last 8 frame addresses

### Rationale

Circular frame chains can occur if corruption creates a loop. A full cycle would cause infinite loop in signal handler. We track the last 8 frames and check for duplicates.

**Why 8?** Balance between detection coverage and cache efficiency. 8 pointers fit in a cache line. Cycles longer than 8 frames are astronomically unlikely from corruption—if memory is that corrupted, pointer validation will catch it.

### Implementation

```c
uintptr_t seen[8] = {0};
int seen_idx = 0;

/* In loop */
for (int i = 0; i < 8 && i < depth; i++) {
    if (seen[i] == (uintptr_t)frame) goto done;
}
seen[seen_idx++ & 7] = (uintptr_t)frame;  /* Rolling overwrite */
```

### Alternatives Considered

- **Hash set**: Heap allocation required—not async-signal-safe
- **Larger fixed array**: More memory per sample; diminishing returns
- **No cycle detection**: Risk of infinite loop

---

## 5. PyCode_Type Caching

### Decision: Cache at module initialization, validate in signal handler

### Rationale

`PyCode_Check(obj)` accesses type object memory. In signal handlers, we cannot safely call Python API. Instead, cache `&PyCode_Type` at init time (single read under GIL) and compare directly in signal handler.

### Safety Analysis

- `PyCode_Type` is a static global in Python runtime—never freed
- Address is constant for lifetime of interpreter
- Comparison is just pointer equality—async-signal-safe

### Implementation

```c
/* At module init (NOT signal context) */
static PyTypeObject *g_cached_code_type = NULL;

void speculative_init(void) {
    g_cached_code_type = &PyCode_Type;
}

/* In signal handler (async-signal-safe) */
static inline int looks_like_code(PyObject *obj) {
    if (!ptr_valid(obj)) return 0;
    return obj->ob_type == g_cached_code_type;
}
```

---

## 6. Tagged Pointer Handling (Python 3.14)

### Decision: Mask low 2 bits before dereferencing

### Rationale

Python 3.14 uses `_PyStackRef` with tagged pointers for deferred reference counting:
- Bit 0: Deferred reference flag
- Bit 1: Reserved

These bits are NOT part of the pointer address. We must clear them to get the actual `PyObject*`.

### Implementation

```c
#define SPPROF_STACKREF_TAG_MASK ((uintptr_t)0x3)

static inline PyObject*
stackref_to_pyobject(_spprof_StackRef *ref) {
    return (PyObject *)(ref->bits & ~SPPROF_STACKREF_TAG_MASK);
}
```

### Alternatives Considered

- **Use Python 3.14's internal macros**: Not available in public headers; coupling risk
- **Different masks per build**: Overly complex; 0x3 mask is documented stable

---

## 7. Statistics Tracking

### Decision: Atomic counters for captured/dropped samples

### Rationale

Users need visibility into profiling accuracy. Two counters:
- `g_samples_captured`: Successfully recorded samples
- `g_samples_dropped_validation`: Samples dropped due to validation failure

Both use `_Atomic uint64_t` with relaxed memory ordering (exact count not critical).

### Implementation

```c
static _Atomic uint64_t g_samples_dropped_validation = 0;

/* In signal handler on validation failure */
atomic_fetch_add_explicit(&g_samples_dropped_validation, 1, memory_order_relaxed);
```

### Exposure

Existing `signal_handler_samples_dropped()` function extended to include validation drops. Python API already exposes this via `Profiler.stats()`.

---

## Summary of Decisions

| Topic | Decision | Key Rationale |
|-------|----------|---------------|
| Safety Model | Speculative + Validation | No coordination overhead, handles rare races |
| Memory Model | Acquire barriers on ARM64 | Ensures visibility of stores on weak-ordered arch |
| Validation | Three-tier (bounds, align, type) | Fast, catches common corruption |
| Cycle Detection | Rolling window of 8 | Cache-efficient, catches loops |
| Type Caching | Cache at init | Async-signal-safe type comparison |
| Tagged Pointers | Mask low 2 bits | Python 3.14 compatibility |
| Statistics | Atomic counters | User visibility into drop rate |

