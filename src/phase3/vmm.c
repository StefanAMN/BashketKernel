/**
 * @file vmm.c
 * @brief Implementation of Virtual Memory Manager and 4-Level Paging
 */

#include "vmm.h"
#include "pmm.h"
#include "kprintf.h"
#include <string.h>

static uint64_t *kernel_pml4 = NULL;

static uint64_t *get_next_level(uint64_t *current_table, size_t index, bool allocate, uint64_t flags) {
    if ((current_table[index] & VMM_PRESENT) && !(current_table[index] & VMM_HUGE)) {
        uint64_t phys = current_table[index] & PTE_ADDR_MASK;
        return (uint64_t *)(phys + pmm_get_hhdm_offset());
    }

    if (!allocate) {
        return NULL;
    }

    void *frame = pmm_alloc_frame();
    if (frame == NULL) {
        kprintf("VMM ERROR: Out of physical memory for page table!\n");
        return NULL;
    }

    uint64_t *table_virt = (uint64_t *)((uint64_t)frame + pmm_get_hhdm_offset());
    for (size_t i = 0; i < 512; i++) {
        table_virt[i] = 0;
    }

    uint64_t entry_flags = VMM_PRESENT | VMM_WRITABLE;
    if (flags & VMM_USER) {
        entry_flags |= VMM_USER;
    }

    current_table[index] = (uint64_t)frame | entry_flags;
    return table_virt;
}

void vmm_init(void) {
    uint64_t cr3;
    asm volatile("mov %0, cr3" : "=r"(cr3));

    uint64_t hhdm = pmm_get_hhdm_offset();
    kernel_pml4 = (uint64_t *)((cr3 & PTE_ADDR_MASK) + hhdm);

    kprintf("VMM Init: Kernel PML4 at Physical: %p, Virtual: %p\n",
            (void *)(cr3 & PTE_ADDR_MASK), kernel_pml4);
}

uint64_t *vmm_get_kernel_pml4(void) {
    return kernel_pml4;
}

bool vmm_map_page(uint64_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags) {
    if (pml4 == NULL) {
        pml4 = kernel_pml4;
    }

    size_t pml4_idx = (virt >> 39) & 0x1FF;
    size_t pdpt_idx = (virt >> 30) & 0x1FF;
    size_t pd_idx   = (virt >> 21) & 0x1FF;
    size_t pt_idx   = (virt >> 12) & 0x1FF;

    uint64_t *pdpt = get_next_level(pml4, pml4_idx, true, flags);
    if (!pdpt) return false;

    uint64_t *pd = get_next_level(pdpt, pdpt_idx, true, flags);
    if (!pd) return false;

    uint64_t *pt = get_next_level(pd, pd_idx, true, flags);
    if (!pt) return false;

    pt[pt_idx] = (phys & PTE_ADDR_MASK) | flags | VMM_PRESENT;
    vmm_invlpg(virt);
    return true;
}

bool vmm_unmap_page(uint64_t *pml4, uint64_t virt) {
    if (pml4 == NULL) {
        pml4 = kernel_pml4;
    }

    size_t pml4_idx = (virt >> 39) & 0x1FF;
    size_t pdpt_idx = (virt >> 30) & 0x1FF;
    size_t pd_idx   = (virt >> 21) & 0x1FF;
    size_t pt_idx   = (virt >> 12) & 0x1FF;

    uint64_t *pdpt = get_next_level(pml4, pml4_idx, false, 0);
    if (!pdpt) return false;

    uint64_t *pd = get_next_level(pdpt, pdpt_idx, false, 0);
    if (!pd) return false;

    uint64_t *pt = get_next_level(pd, pd_idx, false, 0);
    if (!pt) return false;

    pt[pt_idx] = 0;
    vmm_invlpg(virt);
    return true;
}

void vmm_invlpg(uint64_t virt) {
    asm volatile("invlpg [%0]" : : "r"(virt) : "memory");
}

void vmm_switch_pml4(uint64_t *pml4_phys) {
    asm volatile("mov cr3, %0" : : "r"(pml4_phys) : "memory");
}
