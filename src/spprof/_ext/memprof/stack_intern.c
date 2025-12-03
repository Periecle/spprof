/* SPDX-License-Identifier: MIT
 * stack_intern.c - Stack deduplication table
 *
 * Many allocations share the same call site. Interning saves memory and
 * enables O(1) stack comparison via stack_id.
 */

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
    
    /* Ensure hash is non-zero (0 is reserved for empty) */
    if (hash == 0) hash = 1;
    
    size_t capacity = g_memprof.stack_table_capacity;
    uint64_t idx = hash % capacity;
    
    for (int probe = 0; probe < 64; probe++) {
        StackEntry* entry = &g_memprof.stack_table[idx];
        uint64_t entry_hash = atomic_load_explicit(&entry->hash, memory_order_relaxed);
        
        /* Empty slot? Try to claim it */
        if (entry_hash == 0) {
            uint64_t expected = 0;
            if (atomic_compare_exchange_strong_explicit(
                    &entry->hash, &expected, hash,
                    memory_order_acq_rel, memory_order_relaxed)) {
                
                /* Claimed. Fill in native frames */
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
                
                atomic_fetch_add_explicit(&g_memprof.stack_count, 1, memory_order_relaxed);
                
                return (uint32_t)idx;
            }
            
            /* Lost race, re-read hash */
            entry_hash = atomic_load_explicit(&entry->hash, memory_order_relaxed);
        }
        
        /* Check if this is our stack */
        if (entry_hash == hash && entry->depth == depth) {
            /* Probable match - verify frames */
            if (memcmp(entry->frames, frames, (size_t)depth * sizeof(uintptr_t)) == 0) {
                return (uint32_t)idx;  /* Exact match */
            }
        }
        
        /* Collision - linear probe */
        atomic_fetch_add_explicit(&g_memprof.stack_table_collisions, 1, memory_order_relaxed);
        idx = (idx + 1) % capacity;
    }
    
    /* Table full or excessive collisions */
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
    
    /* Verify slot is occupied (hash != 0) */
    if (atomic_load_explicit(&entry->hash, memory_order_relaxed) == 0) {
        return NULL;
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

void stack_table_destroy(void) {
    if (!g_memprof.stack_table) {
        return;
    }
    
    /* Free resolved symbol strings */
    for (size_t i = 0; i < g_memprof.stack_table_capacity; i++) {
        StackEntry* entry = &g_memprof.stack_table[i];
        
        if (entry->function_names) {
            for (int j = 0; j < entry->depth; j++) {
                free(entry->function_names[j]);
            }
            free(entry->function_names);
        }
        
        if (entry->file_names) {
            for (int j = 0; j < entry->depth; j++) {
                free(entry->file_names[j]);
            }
            free(entry->file_names);
        }
        
        free(entry->line_numbers);
    }
    
    size_t size = g_memprof.stack_table_capacity * sizeof(StackEntry);
    
#ifdef _WIN32
    VirtualFree(g_memprof.stack_table, 0, MEM_RELEASE);
#else
    munmap(g_memprof.stack_table, size);
#endif
    
    g_memprof.stack_table = NULL;
    g_memprof.stack_table_capacity = 0;
}

