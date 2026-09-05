/**
 * @file heap.h
 * @brief The kernel heap: kmalloc and kfree
 *
 * Blocks of any size from one stretch of kernel address space starting at
 * KHEAP_START_VIRT, grown a page at a time and given back to the frame
 * allocator when a large stretch at its end is free. Neither call depends
 * on how many blocks are in use (heap.c says how). Safe from any CPU and
 * from interrupt handlers; never sleeps.
 */

#ifndef HEAP_H
#define HEAP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define KHEAP_START_VIRT 0xFFFF900000000000ULL
#define KHEAP_INITIAL_SIZE (1 * 1024 * 1024) // 1 MB initial heap size
#define KHEAP_MAX_ALLOC (1ULL << 30)         // Largest single allocation: 1 GiB
#define KHEAP_TRIM_THRESHOLD (256 * 1024)    // Free space at the end before frames are returned
#define KHEAP_TRIM_KEEP (64 * 1024)          // Free space kept at the end after returning frames

/**
 * @brief Map the heap's first KHEAP_INITIAL_SIZE bytes. Call once, after
 *        the frame allocator and the page tables.
 */
void kheap_init(void);

/**
 * @brief Allocate `size` bytes (not cleared).
 * @return A 16-byte aligned pointer, or NULL for 0 bytes, more than
 *         KHEAP_MAX_ALLOC, or when no frames can be had to grow the heap.
 */
void *kmalloc(size_t size);

/**
 * @brief Give back a block kmalloc returned (NULL is allowed). Freeing a
 *        block twice is reported and ignored.
 */
void kfree(void *ptr);

/**
 * @brief Get total active allocated payload bytes on heap.
 * @return Allocated payload byte count.
 */
size_t kheap_get_allocated_bytes(void);

/**
 * @brief Bytes of address space the heap currently has frames for.
 */
size_t kheap_get_mapped_bytes(void);

/**
 * @brief Number of kmalloc calls refused for lack of memory since boot.
 */
size_t kheap_get_failed_allocations(void);

/**
 * @brief For tests: walk every block and every free list and check that
 *        they agree (sizes chain up, no two free blocks touch, each free
 *        block is on the right list exactly once, the byte count is right,
 *        and every free block still holds its poison).
 */
bool kheap_check(void);

#ifdef KERNEL_TESTS
/**
 * @brief For tests: how many blocks kmalloc has found written after they
 *        were freed (each is a kernel panic unless switched off below).
 */
size_t kheap_poison_damage(void);

/**
 * @brief For tests: whether such a block panics (the default) or is only
 *        counted, so that a test can do it on purpose.
 */
void kheap_poison_damage_is_fatal(bool fatal);
#endif

#endif // HEAP_H
