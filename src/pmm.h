#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "limine.h"

#define PAGE_SIZE 4096

// Initialize Physical Memory Manager from Limine memory map
void pmm_init(struct limine_memmap_response *memmap, uint64_t hhdm_offset);

// Allocate a single 4KiB physical frame (returns physical address)
void *pmm_alloc_frame(void);

// Allocate multiple contiguous 4KiB physical frames (returns physical address)
void *pmm_alloc_frames(size_t count);

// Free a single physical frame
void pmm_free_frame(void *phys_addr);

// Free multiple physical frames
void pmm_free_frames(void *phys_addr, size_t count);

// Getters for memory statistics
size_t pmm_get_total_frames(void);
size_t pmm_get_free_frames(void);
size_t pmm_get_used_frames(void);
uint64_t pmm_get_hhdm_offset(void);

#endif // PMM_H
