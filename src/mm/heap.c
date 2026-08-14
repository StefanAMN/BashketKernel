/**
 * @file heap.c
 * @brief Implementation of Kernel Block-Based Dynamic Heap Allocator
 */

#include "heap.h"
#include "vmm.h"
#include "pmm.h"
#include "kprintf.h"
#include "spinlock.h"

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
static size_t failed_allocations = 0;

// The block list is walked and edited by every kmalloc/kfree, and a timer
// interrupt can switch tasks in the middle of either one.
// Lock order: heap_lock, then the VMM lock, then the PMM lock.
static spinlock_t heap_lock = SPINLOCK_RANKED("heap", LOCK_RANK_HEAP);

// Give the pages in [new_end, heap_end_virt) back to the frame allocator.
static void shrink_heap(uint64_t new_end) {
    while (heap_end_virt > new_end) {
        heap_end_virt -= PAGE_SIZE;
        uint64_t phys = vmm_get_phys(NULL, heap_end_virt);
        vmm_unmap_page(NULL, heap_end_virt);
        if (phys) pmm_free_frame((void *)phys);
    }
}

// Grow the heap by whole pages. All or nothing: if memory runs out part way,
// the pages added so far are returned, so a failed large request does not
// leave the heap sitting on the last free frames of the machine.
static bool expand_heap(size_t bytes) {
    size_t page_count = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t old_end = heap_end_virt;

    for (size_t i = 0; i < page_count; i++) {
        void *phys = pmm_alloc_frame();
        if (phys == NULL) {
            shrink_heap(old_end);
            return false;
        }

        if (!vmm_map_page(NULL, heap_end_virt, (uint64_t)phys, VMM_PRESENT | VMM_WRITABLE)) {
            pmm_free_frame(phys);
            shrink_heap(old_end);
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

size_t kheap_get_mapped_bytes(void) {
    return (size_t)(heap_end_virt - KHEAP_START_VIRT);
}

size_t kheap_get_failed_allocations(void) {
    return failed_allocations;
}

// Caller must hold heap_lock.
static void *kmalloc_locked(size_t size) {

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

    // No free block is large enough: grow the heap at its end. If the last
    // block is free, only the shortfall is needed and the block is extended;
    // otherwise a new block is added after it.
    heap_block_t *last = heap_start;
    while (last->next != NULL) {
        last = last->next;
    }

    size_t required = last->is_free ? aligned_size - last->size : aligned_size + BLOCK_HEADER_SIZE;
    if (!expand_heap(required)) {
        failed_allocations++;
        return NULL;
    }

    if (last->is_free) {
        last->size = (heap_end_virt - (uint64_t)last) - BLOCK_HEADER_SIZE;
    } else {
        heap_block_t *expanded = (heap_block_t *)((uint8_t *)last + BLOCK_HEADER_SIZE + last->size);
        expanded->size = (heap_end_virt - (uint64_t)expanded) - BLOCK_HEADER_SIZE;
        expanded->is_free = true;
        expanded->next = NULL;
        last->next = expanded;
    }

    return kmalloc_locked(size);
}

void *kmalloc(size_t size) {
    if (size == 0 || heap_start == NULL) return NULL;
    if (size > KHEAP_MAX_ALLOC) return NULL; // Also keeps the size arithmetic from overflowing

    uint64_t flags = spinlock_acquire_irqsave(&heap_lock);
    void *ptr = kmalloc_locked(size);
    spinlock_release_irqrestore(&heap_lock, flags);
    return ptr;
}

void kfree(void *ptr) {
    if (ptr == NULL) return;

    uint64_t flags = spinlock_acquire_irqsave(&heap_lock);

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

    // `current` is now the last block. If it is free and large, hand most of
    // it back to the frame allocator: memory freed by the kernel (a deleted
    // file, exited processes) becomes available to programs again.
    if (current != NULL && current->is_free && current->size >= KHEAP_TRIM_THRESHOLD) {
        uint64_t payload = (uint64_t)current + BLOCK_HEADER_SIZE;
        uint64_t new_end = page_align_up(payload + KHEAP_TRIM_KEEP);
        uint64_t floor = KHEAP_START_VIRT + KHEAP_INITIAL_SIZE; // Never below the initial size
        if (new_end < floor) new_end = floor;

        if (new_end < heap_end_virt) {
            shrink_heap(new_end);
            current->size = heap_end_virt - payload;
        }
    }

    spinlock_release_irqrestore(&heap_lock, flags);
}
