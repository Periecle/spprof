/* SPDX-License-Identifier: MIT
 * stack_intern.c - Stack deduplication table
 *
 * Many allocations share the same call site. Interning saves memory and
 * enables O(1) stack comparison via stack_id.
 *
 * ALGORITHM:
 *   Uses open-addressing hash table with linear probing.
 *   Key: FNV-1a hash of frame array
 *   Collision resolution: Linear probe up to 64 slots
 *
 * THREAD SAFETY:
 *   stack_table_intern() uses CAS on hash field for lock-free insertion.
 *   Duplicate inserts by racing threads are harmless (return same ID).
 *
 * MEMORY:
 *   Backing array allocated via mmap/VirtualAlloc (not malloc).
 *   Dynamic resizing supported via stack_table_resize().
 *
 * Copyright (c) 2024 spprof contributors
 */

/* _GNU_SOURCE must be defined BEFORE any system headers for mremap() on Linux */
#if defined(__linux__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#endif

#include "stack_intern.h"
#include "memprof.h"
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

/* ============================================================================
 * FNV-1a Hash
 * ============================================================================ */

uint64_t fnv1a_hash_stack(const uintptr_t* frames, int depth) {
    uint64_t hash = 0xCBF29CE484222325ULL;  /* FNV offset basis */
    
    const uint8_t* data = (const uint8_t*)frames;
    size_t len = (size_t)depth * sizeof(uintptr_t);
    
    for (size_t i = 0; i < len; i++) {
        hash ^= data[i];
        hash *= 0x100000001B3ULL;  /* FNV prime */
    }
    
    return hash;
}

/* ============================================================================
 * Initialization
 * ============================================================================ */

int stack_table_init(void) {
    size_t capacity = MEMPROF_STACK_TABLE_INITIAL;
    size_t size = capacity * sizeof(StackEntry);
    
    /* RESOURCE LEAK FIX: If stack_table already exists (e.g., after shutdown
     * without full cleanup), we need to handle it properly.
     * 
     * Strategy: Free array structures (not string contents which are interned),
     * then clear and reuse. This prevents ~35MB+ leak on profiler restart. */
    if (g_memprof.stack_table != NULL) {
        /* Free resolved symbol array structures before clearing.
         * NOTE: Strings are interned and managed by string_table, not freed here. */
        for (size_t i = 0; i < g_memprof.stack_table_capacity; i++) {
            StackEntry* entry = &g_memprof.stack_table[i];
            free(entry->function_names);
            free(entry->file_names);
            free(entry->line_numbers);
        }
        
        /* If capacity matches, reuse. Otherwise, need to reallocate. */
        if (g_memprof.stack_table_capacity == capacity) {
            memset(g_memprof.stack_table, 0, size);
            atomic_store_explicit(&g_memprof.stack_count, 0, memory_order_relaxed);
            return 0;
        }
        
        /* Capacity changed - free old and allocate new */
        size_t old_size = g_memprof.stack_table_capacity * sizeof(StackEntry);
#ifdef _WIN32
        VirtualFree(g_memprof.stack_table, 0, MEM_RELEASE);
#else
        munmap(g_memprof.stack_table, old_size);
#endif
        g_memprof.stack_table = NULL;
    }
    
#ifdef _WIN32
    g_memprof.stack_table = (StackEntry*)VirtualAlloc(
        NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_memprof.stack_table) {
        return -1;
    }
#else
    g_memprof.stack_table = (StackEntry*)mmap(
        NULL, size,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1, 0);
    if (g_memprof.stack_table == MAP_FAILED) {
        g_memprof.stack_table = NULL;
        return -1;
    }
#endif
    
    /* Zero-initialize (hash=0 means empty slot) */
    memset(g_memprof.stack_table, 0, size);
    
    g_memprof.stack_table_capacity = capacity;
    atomic_store_explicit(&g_memprof.stack_count, 0, memory_order_relaxed);
    
    return 0;
}

/* ============================================================================
 * Interning
 * ============================================================================ */

uint32_t stack_table_intern(const uintptr_t* frames, int depth,
                            const uintptr_t* python_frames, int python_depth) {
    if (!g_memprof.stack_table || depth <= 0) {
        return UINT32_MAX;
    }
    
    /* Clamp depths to max */
    if (depth > MEMPROF_MAX_STACK_DEPTH) {
        depth = MEMPROF_MAX_STACK_DEPTH;
    }
    if (python_depth > MEMPROF_MAX_STACK_DEPTH) {
        python_depth = MEMPROF_MAX_STACK_DEPTH;
    }
    
    uint64_t hash = fnv1a_hash_stack(frames, depth);
    
    /* Ensure hash is >= 2 (0=empty, 1=reserved marker) */
    if (hash < 2) hash = hash + 2;
    
    size_t capacity = g_memprof.stack_table_capacity;
    uint64_t idx = hash % capacity;
    
    for (int probe = 0; probe < 64; probe++) {
        StackEntry* entry = &g_memprof.stack_table[idx];
        uint64_t entry_hash = atomic_load_explicit(&entry->hash, memory_order_acquire);
        
        /* Empty slot? Try to claim it with two-phase insert */
        if (entry_hash == STACK_HASH_EMPTY) {
            uint64_t expected = STACK_HASH_EMPTY;
            
            /*
             * PHASE 1: Reserve the slot (CAS EMPTY → RESERVED)
             *
             * This prevents other writers from claiming this slot while
             * we're filling in the data.
             */
            if (atomic_compare_exchange_strong_explicit(
                    &entry->hash, &expected, STACK_HASH_RESERVED,
                    memory_order_acq_rel, memory_order_relaxed)) {
                
                /*
                 * Slot is now RESERVED. Other threads will see RESERVED and
                 * skip this slot (won't read partial data).
                 *
                 * PHASE 2: Fill in all data BEFORE publishing the hash.
                 */
                entry->depth = (uint16_t)depth;
                entry->flags = 0;
                memcpy(entry->frames, frames, (size_t)depth * sizeof(uintptr_t));
                
                /* Store Python frames if provided */
                if (python_frames && python_depth > 0) {
                    entry->python_depth = (uint16_t)python_depth;
                    memcpy(entry->python_frames, python_frames, 
                           (size_t)python_depth * sizeof(uintptr_t));
                    entry->flags |= STACK_FLAG_PYTHON_ATTR;
                } else {
                    entry->python_depth = 0;
                }
                
                entry->function_names = NULL;
                entry->file_names = NULL;
                entry->line_numbers = NULL;
                
                /*
                 * PHASE 3: Publish the real hash with release semantics.
                 *
                 * This ensures all the data writes above are visible to any
                 * thread that subsequently reads this hash value.
                 */
                atomic_store_explicit(&entry->hash, hash, memory_order_release);
                
                atomic_fetch_add_explicit(&g_memprof.stack_count, 1, memory_order_relaxed);
                
                return (uint32_t)idx;
            }
            
            /* Lost race, re-read hash */
            entry_hash = atomic_load_explicit(&entry->hash, memory_order_acquire);
        }
        
        /* Skip RESERVED slots - another thread is writing, data not ready */
        if (entry_hash == STACK_HASH_RESERVED) {
            /* Could be our stack being written by another thread racing us.
             * Continue probing - if it's ours, we'll find it on a retry.
             * This is safe because duplicate inserts just waste a slot. */
            atomic_fetch_add_explicit(&g_memprof.stack_table_collisions, 1, memory_order_relaxed);
            idx = (idx + 1) % capacity;
            continue;
        }
        
        /* Valid hash (>= 2): Check if this is our stack */
        if (entry_hash == hash && entry->depth == depth) {
            /* Probable match - verify frames.
             * Safe to read entry->frames because hash >= 2 means data is published. */
            if (memcmp(entry->frames, frames, (size_t)depth * sizeof(uintptr_t)) == 0) {
                return (uint32_t)idx;  /* Exact match */
            }
        }
        
        /* Collision - linear probe */
        atomic_fetch_add_explicit(&g_memprof.stack_table_collisions, 1, memory_order_relaxed);
        idx = (idx + 1) % capacity;
    }
    
    /* Table full or excessive collisions.
     * 
     * IMPORTANT: This is a serious condition. All subsequent allocations
     * will have stack_id = UINT32_MAX, leading to broken/missing stacks
     * in the profile output.
     *
     * Attempt resize if not actively profiling (resize is not thread-safe
     * during concurrent interning). If resize fails or is unsafe, we must
     * gracefully degrade.
     */
    
    /* Track saturation event */
    atomic_fetch_add_explicit(&g_memprof.stack_table_saturations, 1,
                              memory_order_relaxed);
    
    /* Only attempt resize if profiling is not active (safe window) */
    if (!atomic_load_explicit(&g_memprof.active_alloc, memory_order_relaxed)) {
        if (stack_table_resize() == 0) {
            /* Resize succeeded - retry interning once */
            /* Note: Simple recursion is safe here since we only retry once */
            uint32_t retry_id = stack_table_intern(frames, depth, python_frames, python_depth);
            if (retry_id != UINT32_MAX) {
                return retry_id;
            }
        }
    }
    
    return UINT32_MAX;
}

/* ============================================================================
 * Lookup
 * ============================================================================ */

const StackEntry* stack_table_get(uint32_t stack_id) {
    if (!g_memprof.stack_table || stack_id >= g_memprof.stack_table_capacity) {
        return NULL;
    }
    
    StackEntry* entry = &g_memprof.stack_table[stack_id];
    
    /* Verify slot is fully written (hash >= 2).
     * EMPTY (0) = slot not used
     * RESERVED (1) = slot being written, data not ready
     * >= 2 = valid, data is safe to read */
    uint64_t hash = atomic_load_explicit(&entry->hash, memory_order_acquire);
    if (hash < 2) {
        return NULL;  /* Empty or reserved - not ready */
    }
    
    return entry;
}

/* ============================================================================
 * Statistics
 * ============================================================================ */

uint32_t stack_table_count(void) {
    return atomic_load_explicit(&g_memprof.stack_count, memory_order_relaxed);
}

size_t stack_table_capacity(void) {
    return g_memprof.stack_table_capacity;
}

int stack_table_load_percent(void) {
    uint32_t count = stack_table_count();
    size_t capacity = stack_table_capacity();
    
    if (capacity == 0) return 0;
    
    return (int)((count * 100) / capacity);
}

int stack_table_needs_resize(void) {
    int load = stack_table_load_percent();
    return load >= MEMPROF_STACK_TABLE_GROW_THRESHOLD;
}

/* ============================================================================
 * Resize (Platform-Specific)
 * ============================================================================ */

int stack_table_resize(void) {
    if (!g_memprof.stack_table) {
        return -1;
    }
    
    /* Check if we've hit max capacity */
    size_t max_capacity = MEMPROF_STACK_TABLE_MAX_DEFAULT;
    
    /* Allow override via environment variable */
    const char* max_env = getenv("SPPROF_STACK_TABLE_MAX");
    if (max_env) {
        unsigned long val = strtoul(max_env, NULL, 10);
        if (val > 0) {
            max_capacity = (size_t)val;
        }
    }
    
    size_t old_capacity = g_memprof.stack_table_capacity;
    size_t new_capacity = old_capacity * 2;
    
    if (new_capacity > max_capacity) {
        new_capacity = max_capacity;
    }
    
    if (new_capacity <= old_capacity) {
        return -1;  /* Can't grow further */
    }
    
    size_t old_size = old_capacity * sizeof(StackEntry);
    size_t new_size = new_capacity * sizeof(StackEntry);
    
#ifdef __linux__
    /* Linux: Use mremap for efficient in-place growth */
    void* new_table = mremap(g_memprof.stack_table, old_size, new_size, MREMAP_MAYMOVE);
    if (new_table == MAP_FAILED) {
        return -1;
    }
    
    /* Zero-initialize new entries */
    memset((char*)new_table + old_size, 0, new_size - old_size);
    
    g_memprof.stack_table = (StackEntry*)new_table;
    g_memprof.stack_table_capacity = new_capacity;
    
#else
    /* macOS/Windows: Allocate new + copy + free old */
    StackEntry* new_table;
    
#ifdef _WIN32
    new_table = (StackEntry*)VirtualAlloc(
        NULL, new_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!new_table) {
        return -1;
    }
#else
    new_table = (StackEntry*)mmap(
        NULL, new_size,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1, 0);
    if (new_table == MAP_FAILED) {
        return -1;
    }
#endif
    
    /* Zero-initialize then copy old entries */
    memset(new_table, 0, new_size);
    memcpy(new_table, g_memprof.stack_table, old_size);
    
    /* Swap and free old */
    StackEntry* old_table = g_memprof.stack_table;
    g_memprof.stack_table = new_table;
    g_memprof.stack_table_capacity = new_capacity;
    
#ifdef _WIN32
    VirtualFree(old_table, 0, MEM_RELEASE);
#else
    munmap(old_table, old_size);
#endif
    
#endif  /* __linux__ */
    
    return 0;
}

/* ============================================================================
 * Cleanup
 * ============================================================================ */

/* Forward declaration for string table cleanup */
extern void string_table_destroy(void);

void stack_table_destroy(void) {
    if (!g_memprof.stack_table) {
        return;
    }
    
    /* Free resolved symbol array structures.
     * NOTE: The actual strings (function_names[i], file_names[i]) are NOT freed
     * because they're interned in the global string table and shared across
     * multiple stack entries. The string table is cleaned up separately. */
    for (size_t i = 0; i < g_memprof.stack_table_capacity; i++) {
        StackEntry* entry = &g_memprof.stack_table[i];
        
        /* Free the arrays themselves, but NOT the strings they point to */
        free(entry->function_names);
        free(entry->file_names);
        free(entry->line_numbers);
        
        entry->function_names = NULL;
        entry->file_names = NULL;
        entry->line_numbers = NULL;
    }
    
    /* Clean up the interned strings */
    string_table_destroy();
    
    size_t size = g_memprof.stack_table_capacity * sizeof(StackEntry);
    
#ifdef _WIN32
    VirtualFree(g_memprof.stack_table, 0, MEM_RELEASE);
#else
    munmap(g_memprof.stack_table, size);
#endif
    
    g_memprof.stack_table = NULL;
    g_memprof.stack_table_capacity = 0;
}

