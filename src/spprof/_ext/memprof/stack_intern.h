/* SPDX-License-Identifier: MIT
 * stack_intern.h - Stack deduplication table
 *
 * Many allocations share the same call site. Interning saves memory and
 * enables O(1) stack comparison via stack_id. The table uses lock-free
 * CAS operations for concurrent insertion.
 *
 * THREAD SAFETY:
 *   stack_table_intern() is thread-safe using CAS on the hash field.
 *   Duplicate insertions are harmless (same stack → same ID).
 *
 * MEMORY MANAGEMENT:
 *   Uses mmap/VirtualAlloc for backing memory (not malloc).
 *   Supports dynamic resizing:
 *     - Linux: mremap() for efficient in-place growth
 *     - macOS/Windows: allocate new + copy + free old
 *
 * PLATFORM SUPPORT:
 *   - Linux: mmap, mremap
 *   - macOS: mmap
 *   - Windows: VirtualAlloc, VirtualFree
 *
 * Copyright (c) 2024 spprof contributors
 */

#ifndef SPPROF_STACK_INTERN_H
#define SPPROF_STACK_INTERN_H

/* _GNU_SOURCE for mremap() on Linux - must be before any system headers */
#if defined(__linux__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#endif

#include "memprof.h"
#include <stdint.h>
#include <stddef.h>

/* ============================================================================
 * Stack Intern Table API
 * ============================================================================ */

/**
 * Initialize the stack intern table.
 *
 * Initial capacity: MEMPROF_STACK_TABLE_INITIAL (4K entries)
 * Maximum capacity: Configurable via SPPROF_STACK_TABLE_MAX env var
 *
 * @return 0 on success, -1 on error
 */
int stack_table_init(void);

/**
 * Intern a stack trace, returning a unique 32-bit ID.
 *
 * Lock-free: Uses CAS on hash field.
 * May insert duplicate if two threads race (harmless).
 *
 * @param frames        Array of native return addresses
 * @param depth         Number of native frames
 * @param python_frames Array of Python code object pointers (or NULL)
 * @param python_depth  Number of Python frames (or 0)
 * @return Stack ID (index), or UINT32_MAX if full
 */
uint32_t stack_table_intern(const uintptr_t* frames, int depth,
                            const uintptr_t* python_frames, int python_depth);

/**
 * Get a stack entry by ID.
 *
 * @param stack_id  Stack ID from stack_table_intern()
 * @return Pointer to StackEntry, or NULL if invalid
 */
const StackEntry* stack_table_get(uint32_t stack_id);

/**
 * Get current number of unique stacks.
 *
 * @return Number of interned stacks
 */
uint32_t stack_table_count(void);

/**
 * Get current capacity of the stack table.
 *
 * @return Current capacity (number of slots)
 */
size_t stack_table_capacity(void);

/**
 * Get load factor as percentage.
 *
 * @return Load factor (0-100)
 */
int stack_table_load_percent(void);

/**
 * Check if the stack table needs resizing.
 *
 * @return 1 if resize needed, 0 otherwise
 */
int stack_table_needs_resize(void);

/**
 * Resize the stack table (called when load > threshold).
 *
 * Platform-specific implementation:
 * - Linux: mremap() for efficient in-place growth
 * - macOS/Windows: mmap new + memcpy + munmap old
 *
 * @return 0 on success, -1 on error
 */
int stack_table_resize(void);

/**
 * Free stack table resources.
 */
void stack_table_destroy(void);

/* ============================================================================
 * Hash Functions
 * ============================================================================ */

/**
 * FNV-1a hash for stack frames.
 *
 * @param frames  Array of return addresses
 * @param depth   Number of frames
 * @return 64-bit hash value
 */
uint64_t fnv1a_hash_stack(const uintptr_t* frames, int depth);

#endif /* SPPROF_STACK_INTERN_H */

