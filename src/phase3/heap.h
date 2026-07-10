/**
 * @file heap.h
 * @brief Kernel Dynamic Heap Allocator (`kmalloc` / `kfree`) Interface
 * 
 * Provides dynamic memory allocation on the kernel heap starting at virtual 
 * address `0xFFFF900000000000`.
 */

#ifndef HEAP_H
#define HEAP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define KHEAP_START_VIRT 0xFFFF900000000000ULL
#define KHEAP_INITIAL_SIZE (1 * 1024 * 1024) // 1 MB initial heap size

/**
 * @brief Initialize Kernel Heap Manager.
 * Maps initial virtual pages via VMM and PMM.
 */
void kheap_init(void);

/**
 * @brief Allocate dynamic memory from kernel heap.
 * @param size Number of payload bytes requested.
 * @return 16-byte aligned pointer to allocated memory block, or NULL if OOM.
 */
void *kmalloc(size_t size);

/**
 * @brief Free previously allocated heap block.
 * @param ptr Pointer to payload block returned by `kmalloc`.
 */
void kfree(void *ptr);

/**
 * @brief Get total active allocated payload bytes on heap.
 * @return Allocated payload byte count.
 */
size_t kheap_get_allocated_bytes(void);

#endif // HEAP_H
