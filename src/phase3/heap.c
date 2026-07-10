/**
 * @file heap.c
 * @brief Implementation of Kernel Block-Based Dynamic Heap Allocator
 */

#include "heap.h"
#include "vmm.h"
#include "pmm.h"
#include "kprintf.h"

typedef struct heap_block {
    size_t size;               // Payload size in bytes
    bool is_free;              // Availability flag
    struct heap_block *next;   // Pointer to next heap block
} heap_block_t;

#define ALIGNMENT 16
#define ALIGN(size) (((size) + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1))
#define BLOCK_HEADER_SIZE ALIGN(sizeof(heap_block_t))

static heap_block_t *heap_start = NULL;
static uint64_t heap_end_virt = KHEAP_START_VIRT;
static size_t total_allocated_bytes = 0;

static bool expand_heap(size_t bytes) {
    size_t page_count = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    for (size_t i = 0; i < page_count; i++) {
        void *phys = pmm_alloc_frame();
        if (phys == NULL) {
            kprintf("KHEAP ERROR: Could not allocate frame to expand heap!\n");
            return false;
        }

        if (!vmm_map_page(NULL, heap_end_virt, (uint64_t)phys, VMM_PRESENT | VMM_WRITABLE)) {
            kprintf("KHEAP ERROR: Could not map page at %p!\n", (void *)heap_end_virt);
            pmm_free_frame(phys);
            return false;
        }

        heap_end_virt += PAGE_SIZE;
    }

    return true;
}

void kheap_init(void) {
    if (!expand_heap(KHEAP_INITIAL_SIZE)) {
        kprintf("KHEAP ERROR: Failed to initialize kernel heap!\n");
        return;
    }

    heap_start = (heap_block_t *)KHEAP_START_VIRT;
    heap_start->size = KHEAP_INITIAL_SIZE - BLOCK_HEADER_SIZE;
    heap_start->is_free = true;
    heap_start->next = NULL;

    kprintf("KHEAP Init: Virtual Range: %p - %p (Initial %zu KB)\n",
            (void *)KHEAP_START_VIRT, (void *)heap_end_virt, KHEAP_INITIAL_SIZE / 1024);
}

size_t kheap_get_allocated_bytes(void) {
    return total_allocated_bytes;
}

void *kmalloc(size_t size) {
    if (size == 0) return NULL;

    size_t aligned_size = ALIGN(size);
    heap_block_t *current = heap_start;

    // Search free list for first fit block
    while (current != NULL) {
        if (current->is_free && current->size >= aligned_size) {
            // Split block if remaining space can hold another block
            if (current->size >= aligned_size + BLOCK_HEADER_SIZE + ALIGNMENT) {
                heap_block_t *new_block = (heap_block_t *)((uint8_t *)current + BLOCK_HEADER_SIZE + aligned_size);
                new_block->size = current->size - aligned_size - BLOCK_HEADER_SIZE;
                new_block->is_free = true;
                new_block->next = current->next;

                current->size = aligned_size;
                current->next = new_block;
            }

            current->is_free = false;
            total_allocated_bytes += current->size;
            return (void *)((uint8_t *)current + BLOCK_HEADER_SIZE);
        }

        current = current->next;
    }

    // Expand heap if no existing free block is large enough
    size_t required = aligned_size + BLOCK_HEADER_SIZE;
    heap_block_t *prev_end = heap_start;
    while (prev_end->next != NULL) {
        prev_end = prev_end->next;
    }

    if (!expand_heap(required)) {
        kprintf("KHEAP ERROR: kmalloc failed for %zu bytes!\n", size);
        return NULL;
    }

    heap_block_t *expanded = (heap_block_t *)((uint8_t *)prev_end + BLOCK_HEADER_SIZE + prev_end->size);
    expanded->size = (heap_end_virt - (uint64_t)expanded) - BLOCK_HEADER_SIZE;
    expanded->is_free = true;
    expanded->next = NULL;
    prev_end->next = expanded;

    return kmalloc(size);
}

void kfree(void *ptr) {
    if (ptr == NULL) return;

    heap_block_t *block = (heap_block_t *)((uint8_t *)ptr - BLOCK_HEADER_SIZE);
    block->is_free = true;
    if (total_allocated_bytes >= block->size) {
        total_allocated_bytes -= block->size;
    }

    // Coalesce adjacent free blocks in heap list
    heap_block_t *current = heap_start;
    while (current != NULL && current->next != NULL) {
        if (current->is_free && current->next->is_free) {
            current->size += BLOCK_HEADER_SIZE + current->next->size;
            current->next = current->next->next;
        } else {
            current = current->next;
        }
    }
}
