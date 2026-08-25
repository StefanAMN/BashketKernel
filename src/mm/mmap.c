/**
 * @file mmap.c
 * @brief Implementation of Virtual Memory Allocation (mmap / munmap)
 */

#include "mmap.h"
#include "vmm.h"
#include "pmm.h"
#include "kprintf.h"
#include "string_util.h"

static uint64_t next_user_mmap_addr = 0x0000700000000000ULL;

void *mmap(void *addr, size_t length, int prot, int flags, int fd, int64_t offset) {
    (void)fd;
    (void)offset;

    if (length == 0) return MAP_FAILED;

    size_t page_count = (length + PAGE_SIZE - 1) / PAGE_SIZE;

    uint64_t target_vaddr = (uint64_t)addr;
    if (target_vaddr == 0) {
        target_vaddr = next_user_mmap_addr;
        next_user_mmap_addr += page_count * PAGE_SIZE;
    }

    uint64_t pte_flags = VMM_PRESENT | VMM_USER;
    if (prot & PROT_WRITE) {
        pte_flags |= VMM_WRITABLE;
    }

    for (size_t i = 0; i < page_count; i++) {
        uint64_t vaddr = target_vaddr + (i * PAGE_SIZE);

        void *phys_frame = NULL;
        if (flags & MAP_ANONYMOUS) {
            phys_frame = pmm_alloc_frame();
            if (!phys_frame) return MAP_FAILED;
            
            // Zero out physical frame contents
            uint64_t hhdm = pmm_get_hhdm_offset();
            memset((void *)((uint64_t)phys_frame + hhdm), 0, PAGE_SIZE);
        }

        vmm_map_page(NULL, vaddr, (uint64_t)phys_frame, pte_flags);
    }

    return (void *)target_vaddr;
}

int munmap(void *addr, size_t length) {
    if (!addr || length == 0) return -1;

    size_t page_count = (length + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t start_vaddr = (uint64_t)addr;

    for (size_t i = 0; i < page_count; i++) {
        uint64_t vaddr = start_vaddr + (i * PAGE_SIZE);
        vmm_unmap_page(NULL, vaddr);
    }

    return 0;
}
