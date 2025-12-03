/* SPDX-License-Identifier: MIT
 * heap_map.c - Lock-free heap map for sampled allocations
 *
 * This implements a lock-free hash table using open addressing with linear
 * probing. The key insight is a two-phase insert (reserve→finalize) that
 * prevents the "free-before-insert" race condition.
 */

#include "heap_map.h"
#include "memprof.h"
#include <string.h>
#include <errno.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

/* ============================================================================
 * Initialization
 * ============================================================================ */

int heap_map_init(void) {
    size_t size = MEMPROF_HEAP_MAP_CAPACITY * sizeof(HeapMapEntry);
    
#ifdef _WIN32
    g_memprof.heap_map = (HeapMapEntry*)VirtualAlloc(
        NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_memprof.heap_map) {
        return -1;
    }
#else
    g_memprof.heap_map = (HeapMapEntry*)mmap(
        NULL, size,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1, 0);
    if (g_memprof.heap_map == MAP_FAILED) {
        g_memprof.heap_map = NULL;
        return -1;
    }
#endif

    /* mmap returns zero-initialized memory on most platforms,
     * but let's be explicit for portability */
    memset(g_memprof.heap_map, 0, size);
    
    return 0;
}

/* ============================================================================
 * Two-Phase Insert: Reserve
 * ============================================================================ */

int heap_map_reserve(uintptr_t ptr) {
    uint64_t idx = heap_map_hash_ptr(ptr) & MEMPROF_HEAP_MAP_MASK;
    
    for (int probe = 0; probe < MEMPROF_MAX_PROBE; probe++) {
        HeapMapEntry* entry = &g_memprof.heap_map[idx];
        uintptr_t current = atomic_load_explicit(&entry->ptr, memory_order_relaxed);
        
        /* Try to claim EMPTY or TOMBSTONE slots */
        if (current == HEAP_ENTRY_EMPTY || current == HEAP_ENTRY_TOMBSTONE) {
            uintptr_t expected = current;
            if (atomic_compare_exchange_strong_explicit(
                    &entry->ptr, &expected, HEAP_ENTRY_RESERVED,
                    memory_order_acq_rel, memory_order_relaxed)) {
                
                /* Slot claimed. Store ptr temporarily in metadata for matching
                 * during "death during birth" detection. */
                atomic_store_explicit(&entry->metadata, (uint64_t)ptr,
                                      memory_order_release);
                
                /* Track tombstone recycling for diagnostics */
                if (current == HEAP_ENTRY_TOMBSTONE) {
                    atomic_fetch_add_explicit(&g_memprof.tombstones_recycled, 1,
                                              memory_order_relaxed);
                }
                
                atomic_fetch_add_explicit(&g_memprof.heap_map_insertions, 1,
                                          memory_order_relaxed);
                
                return (int)idx;  /* Return slot index for finalize */
            }
            /* CAS failed - another thread claimed it, continue probing */
        }
        
        /* Track collision */
        atomic_fetch_add_explicit(&g_memprof.heap_map_collisions, 1,
                                  memory_order_relaxed);
        
        idx = (idx + 1) & MEMPROF_HEAP_MAP_MASK;
    }
    
    /* Table full (all probed slots are OCCUPIED or RESERVED) */
    atomic_fetch_add_explicit(&g_memprof.heap_map_full_drops, 1,
                              memory_order_relaxed);
    return -1;
}

/* ============================================================================
 * Two-Phase Insert: Finalize
 * ============================================================================ */

int heap_map_finalize(int slot_idx, uintptr_t ptr, uint32_t stack_id,
                      uint32_t size, uint32_t weight, uint64_t birth_seq,
                      uint64_t timestamp) {
    if (slot_idx < 0 || slot_idx >= MEMPROF_HEAP_MAP_CAPACITY) {
        return 0;
    }
    
    HeapMapEntry* entry = &g_memprof.heap_map[slot_idx];
    
    /* Clamp size to 24-bit max (16MB) */
    if (size > MAX_ALLOC_SIZE) {
        size = MAX_ALLOC_SIZE;
    }
    
    /* Pack metadata */
    uint64_t packed_metadata = METADATA_PACK(stack_id, size, weight);
    
    /* Store metadata first (relaxed OK, ptr publish provides release) */
    atomic_store_explicit(&entry->metadata, packed_metadata, memory_order_relaxed);
    atomic_store_explicit(&entry->birth_seq, birth_seq, memory_order_relaxed);
    entry->timestamp = timestamp;  /* Non-atomic, protected by state transition */
    
    /* Publish: CAS RESERVED → ptr. If this fails, "death during birth" occurred. */
    uintptr_t expected = HEAP_ENTRY_RESERVED;
    if (!atomic_compare_exchange_strong_explicit(
            &entry->ptr, &expected, ptr,
            memory_order_release, memory_order_relaxed)) {
        
        /* Slot was tombstoned by free() - allocation died during birth.
         * Clean up: entry is already TOMBSTONE, just update stats. */
        atomic_fetch_sub_explicit(&g_memprof.heap_map_insertions, 1,
                                  memory_order_relaxed);
        atomic_fetch_add_explicit(&g_memprof.death_during_birth, 1,
                                  memory_order_relaxed);
        return 0;  /* Indicate birth failure */
    }
    
    return 1;  /* Success */
}

/* ============================================================================
 * Remove (Free Path)
 * ============================================================================ */

int heap_map_remove(uintptr_t ptr, uint64_t free_seq, uint64_t free_timestamp,
                    uint32_t* out_stack_id, uint32_t* out_size,
                    uint32_t* out_weight, uint64_t* out_duration) {
    uint64_t idx = heap_map_hash_ptr(ptr) & MEMPROF_HEAP_MAP_MASK;
    
    for (int probe = 0; probe < MEMPROF_MAX_PROBE; probe++) {
        HeapMapEntry* entry = &g_memprof.heap_map[idx];
        uintptr_t entry_ptr = atomic_load_explicit(&entry->ptr, memory_order_acquire);
        
        /* Found it? */
        if (entry_ptr == ptr) {
            /* But is this the SAME allocation we freed, or a new one that
             * reused the address? (macOS "Zombie Killer" race)
             *
             * On macOS malloc_logger, we're a POST-HOOK: real_free() already
             * returned, so the address could have been reused by another thread's
             * malloc() before our handle_free() runs.
             *
             * DETERMINISTIC SOLUTION: Use global sequence counter.
             * If entry->birth_seq > free_seq, this allocation was BORN after
             * our free was issued, so it's a different allocation entirely.
             */
            uint64_t entry_birth_seq = atomic_load_explicit(&entry->birth_seq,
                                                            memory_order_relaxed);
            if (entry_birth_seq > free_seq) {
                /* Entry was created AFTER our free was issued - zombie race!
                 * This is a new allocation, not the one we freed. */
                atomic_fetch_add_explicit(&g_memprof.zombie_races_detected, 1,
                                          memory_order_relaxed);
                return 0;  /* Ignore this zombie free */
            }
            
            /* Safe to remove - normal removal path */
            
            /* Extract metadata for caller */
            uint64_t metadata = atomic_load_explicit(&entry->metadata,
                                                      memory_order_relaxed);
            if (out_stack_id) *out_stack_id = METADATA_STACK_ID(metadata);
            if (out_size)     *out_size = METADATA_SIZE(metadata);
            if (out_weight)   *out_weight = METADATA_WEIGHT(metadata);
            if (out_duration) {
                uint64_t entry_ts = entry->timestamp;
                *out_duration = (free_timestamp > entry_ts) ?
                                (free_timestamp - entry_ts) : 0;
            }
            
            /* Mark as tombstone */
            atomic_store_explicit(&entry->ptr, HEAP_ENTRY_TOMBSTONE,
                                  memory_order_release);
            
            atomic_fetch_add_explicit(&g_memprof.heap_map_deletions, 1,
                                      memory_order_relaxed);
            atomic_fetch_add_explicit(&g_memprof.total_frees_tracked, 1,
                                      memory_order_relaxed);
            
            return 1;
        }
        
        /* Check if this RESERVED slot is for our ptr (stored in metadata) */
        if (entry_ptr == HEAP_ENTRY_RESERVED) {
            uint64_t reserved_ptr = atomic_load_explicit(&entry->metadata,
                                                          memory_order_acquire);
            if (reserved_ptr == (uint64_t)ptr) {
                /* "Death during birth" - tombstone the RESERVED slot.
                 * The allocating thread's finalize() will see this and clean up. */
                atomic_store_explicit(&entry->ptr, HEAP_ENTRY_TOMBSTONE,
                                      memory_order_release);
                
                atomic_fetch_add_explicit(&g_memprof.death_during_birth, 1,
                                          memory_order_relaxed);
                atomic_fetch_add_explicit(&g_memprof.total_frees_tracked, 1,
                                          memory_order_relaxed);
                
                return 1;  /* Successfully "freed" the in-flight allocation */
            }
        }
        
        /* Empty slot means not found */
        if (entry_ptr == HEAP_ENTRY_EMPTY) {
            return 0;  /* Not found (wasn't sampled) */
        }
        
        idx = (idx + 1) & MEMPROF_HEAP_MAP_MASK;
    }
    
    return 0;  /* Not found after max probes */
}

/* ============================================================================
 * Lookup (Read-Only)
 * ============================================================================ */

const HeapMapEntry* heap_map_lookup(uintptr_t ptr) {
    uint64_t idx = heap_map_hash_ptr(ptr) & MEMPROF_HEAP_MAP_MASK;
    
    for (int probe = 0; probe < MEMPROF_MAX_PROBE; probe++) {
        HeapMapEntry* entry = &g_memprof.heap_map[idx];
        uintptr_t entry_ptr = atomic_load_explicit(&entry->ptr, memory_order_acquire);
        
        if (entry_ptr == ptr) {
            return entry;
        }
        
        if (entry_ptr == HEAP_ENTRY_EMPTY) {
            return NULL;
        }
        
        idx = (idx + 1) & MEMPROF_HEAP_MAP_MASK;
    }
    
    return NULL;
}

/* ============================================================================
 * Statistics
 * ============================================================================ */

int heap_map_load_percent(void) {
    uint64_t insertions = atomic_load_explicit(&g_memprof.heap_map_insertions,
                                                memory_order_relaxed);
    uint64_t deletions = atomic_load_explicit(&g_memprof.heap_map_deletions,
                                               memory_order_relaxed);
    
    uint64_t live = (insertions > deletions) ? (insertions - deletions) : 0;
    
    return (int)((live * 100) / MEMPROF_HEAP_MAP_CAPACITY);
}

size_t heap_map_live_count(void) {
    if (!g_memprof.heap_map) {
        return 0;
    }
    
    size_t count = 0;
    for (size_t i = 0; i < MEMPROF_HEAP_MAP_CAPACITY; i++) {
        uintptr_t ptr = atomic_load_explicit(&g_memprof.heap_map[i].ptr,
                                              memory_order_relaxed);
        if (heap_map_is_valid_ptr(ptr)) {
            count++;
        }
    }
    return count;
}

/* ============================================================================
 * Iteration
 * ============================================================================ */

size_t heap_map_iterate(heap_map_iter_fn callback, void* user_data) {
    if (!g_memprof.heap_map || !callback) {
        return 0;
    }
    
    size_t count = 0;
    for (size_t i = 0; i < MEMPROF_HEAP_MAP_CAPACITY; i++) {
        HeapMapEntry* entry = &g_memprof.heap_map[i];
        uintptr_t ptr = atomic_load_explicit(&entry->ptr, memory_order_acquire);
        
        if (heap_map_is_valid_ptr(ptr)) {
            callback(entry, user_data);
            count++;
        }
    }
    return count;
}

/* ============================================================================
 * Cleanup
 * ============================================================================ */

void heap_map_destroy(void) {
    if (g_memprof.heap_map) {
        size_t size = MEMPROF_HEAP_MAP_CAPACITY * sizeof(HeapMapEntry);
        
#ifdef _WIN32
        VirtualFree(g_memprof.heap_map, 0, MEM_RELEASE);
#else
        munmap(g_memprof.heap_map, size);
#endif
        
        g_memprof.heap_map = NULL;
    }
}

