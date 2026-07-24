#ifndef HEAP_H
#define HEAP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Initial heap parameters
#define KHEAP_START_VIRT 0xFFFF900000000000ULL
#define KHEAP_INITIAL_SIZE (1 * 1024 * 1024) // 1 MB initial heap

// Initialize Kernel Heap Allocator
void kheap_init(void);

// Allocate dynamic memory on the kernel heap
void *kmalloc(size_t size);

// Free previously allocated heap memory
void kfree(void *ptr);

// Get total allocated payload bytes on heap
size_t kheap_get_allocated_bytes(void);

#endif // HEAP_H
