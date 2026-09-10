/**
 * @file vmm.c
 * @brief Implementation of Virtual Memory Manager and 4-Level Paging
 */

#include "vmm.h"
#include "pmm.h"
#include "kprintf.h"
#include "string_util.h"
#include "spinlock.h"
#include "cow.h"

static uint64_t *kernel_pml4 = NULL;

// Protects page-table edits. Lock order: vmm_lock, then the PMM lock.
static spinlock_t vmm_lock = SPINLOCK_INIT;

static uint64_t *get_next_level(uint64_t *current_table, size_t index, bool allocate, uint64_t flags) {
    if ((current_table[index] & VMM_PRESENT) && (current_table[index] & VMM_HUGE)) {
        // A 2 MiB or 1 GiB mapping already covers this address. Replacing it
        // with an empty table would silently unmap everything else it covers.
        return NULL;
    }

    if ((current_table[index] & VMM_PRESENT) && !(current_table[index] & VMM_HUGE)) {
        if (flags & VMM_USER) {
            current_table[index] |= VMM_USER;
        }
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

    // Give every kernel-half PML4 slot a page directory pointer table now.
    // Each process gets its own PML4 whose upper 256 entries are copied from
    // this one; if a kernel-half entry were created later (a new heap or
    // stack region), the copies would not see it. With all 256 present from
    // the start, the top-level kernel entries never change again and every
    // address space shares the same lower-level kernel tables.
    for (size_t i = 256; i < 512; i++) {
        if (kernel_pml4[i] & VMM_PRESENT) continue;

        void *frame = pmm_alloc_frame();
        if (!frame) panic("VMM: out of memory preallocating kernel page tables");
        uint64_t *table = (uint64_t *)((uint64_t)frame + hhdm);
        for (size_t e = 0; e < 512; e++) table[e] = 0;
        kernel_pml4[i] = (uint64_t)frame | VMM_PRESENT | VMM_WRITABLE;
    }

    kprintf("VMM Init: Kernel PML4 at Physical: %p, Virtual: %p\n",
            (void *)(cr3 & PTE_ADDR_MASK), kernel_pml4);
}

uint64_t vmm_get_kernel_pml4_phys(void) {
    return (uint64_t)kernel_pml4 - pmm_get_hhdm_offset();
}

uint64_t *vmm_space_from_phys(uint64_t pml4_phys) {
    return (uint64_t *)(pml4_phys + pmm_get_hhdm_offset());
}

uint64_t *vmm_create_address_space(void) {
    void *frame = pmm_alloc_frame();
    if (!frame) return NULL;

    uint64_t *pml4 = (uint64_t *)((uint64_t)frame + pmm_get_hhdm_offset());
    for (size_t i = 0; i < 256; i++) pml4[i] = 0;                // Empty user half
    for (size_t i = 256; i < 512; i++) pml4[i] = kernel_pml4[i]; // Shared kernel half
    return pml4;
}

// Free one level of user page tables and everything below it.
// level 3 = PDPT, 2 = PD, 1 = PT (whose entries are the data frames).
static void free_table(uint64_t *table, int level) {
    uint64_t hhdm = pmm_get_hhdm_offset();

    for (size_t i = 0; i < 512; i++) {
        uint64_t entry = table[i];
        if (!(entry & VMM_PRESENT)) continue;

        void *frame = (void *)(entry & PTE_ADDR_MASK);
        if (level > 1 && !(entry & VMM_HUGE)) {
            free_table((uint64_t *)((uint64_t)frame + hhdm), level - 1);
            pmm_free_frame(frame);
        } else if (level == 1) {
            pmm_unref_frame(frame); // Frees it unless another space shares it
        }
    }
}

void vmm_destroy_address_space(uint64_t *pml4) {
    if (!pml4 || pml4 == kernel_pml4) return;
    uint64_t hhdm = pmm_get_hhdm_offset();

    // Only the user half belongs to this address space
    for (size_t i = 0; i < 256; i++) {
        if (!(pml4[i] & VMM_PRESENT)) continue;
        void *pdpt = (void *)(pml4[i] & PTE_ADDR_MASK);
        free_table((uint64_t *)((uint64_t)pdpt + hhdm), 3);
        pmm_free_frame(pdpt);
    }

    pmm_free_frame((void *)((uint64_t)pml4 - hhdm));
}

// Return a pointer to the page-table entry for `virt`, or NULL if any level
// above it is missing. Caller must hold vmm_lock.
static uint64_t *walk_to_pte(uint64_t *pml4, uint64_t virt) {
    uint64_t *pdpt = get_next_level(pml4, (virt >> 39) & 0x1FF, false, 0);
    uint64_t *pd = pdpt ? get_next_level(pdpt, (virt >> 30) & 0x1FF, false, 0) : NULL;
    uint64_t *pt = pd ? get_next_level(pd, (virt >> 21) & 0x1FF, false, 0) : NULL;
    return pt ? &pt[(virt >> 12) & 0x1FF] : NULL;
}

uint64_t vmm_get_pte(uint64_t *pml4, uint64_t virt) {
    if (pml4 == NULL) pml4 = kernel_pml4;

    uint64_t irq_flags = spinlock_acquire_irqsave(&vmm_lock);
    uint64_t *pte = walk_to_pte(pml4, virt);
    uint64_t value = pte ? *pte : 0;
    spinlock_release_irqrestore(&vmm_lock, irq_flags);
    return value;
}

void *vmm_alloc_user_page(uint64_t *pml4, uint64_t virt, uint64_t flags) {
    if (!pml4 || virt >= USER_SPACE_END) return NULL;
    virt &= ~(uint64_t)(PAGE_SIZE - 1);

    void *frame = pmm_alloc_frame();
    if (!frame) return NULL;

    uint8_t *kernel_view = (uint8_t *)((uint64_t)frame + pmm_get_hhdm_offset());
    memset(kernel_view, 0, PAGE_SIZE);

    if (!vmm_map_page(pml4, virt, (uint64_t)frame, flags | VMM_PRESENT | VMM_USER)) {
        pmm_free_frame(frame);
        return NULL;
    }
    return kernel_view;
}

bool vmm_user_range_ok(uint64_t *pml4, uint64_t addr, size_t len, bool write) {
    if (!pml4) return false;
    if (len == 0) return true;

    uint64_t end = addr + len;
    if (end < addr || end > USER_SPACE_END) return false; // Wraps, or reaches kernel space

    for (uint64_t page = addr & ~(uint64_t)(PAGE_SIZE - 1); page < end; page += PAGE_SIZE) {
        uint64_t pte = vmm_get_pte(pml4, page);
        if (!(pte & VMM_PRESENT) || !(pte & VMM_USER)) return false;
        if (write && !(pte & VMM_WRITABLE)) return false;
    }
    return true;
}

size_t vmm_copy_to_space(uint64_t *pml4, uint64_t dest, const void *src, size_t len) {
    const uint8_t *in = src;
    size_t done = 0;
    uint64_t hhdm = pmm_get_hhdm_offset();

    while (done < len) {
        uint64_t virt = dest + done;
        uint64_t phys = vmm_get_phys(pml4, virt & ~(uint64_t)(PAGE_SIZE - 1));
        if (!phys) break;

        size_t page_off = virt & (PAGE_SIZE - 1);
        size_t chunk = PAGE_SIZE - page_off;
        if (chunk > len - done) chunk = len - done;

        memcpy((void *)(phys + hhdm + page_off), in + done, chunk);
        done += chunk;
    }
    return done;
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

    uint64_t irq_flags = spinlock_acquire_irqsave(&vmm_lock);
    bool ok = false;

    uint64_t *pdpt = get_next_level(pml4, pml4_idx, true, flags);
    uint64_t *pd = pdpt ? get_next_level(pdpt, pdpt_idx, true, flags) : NULL;
    uint64_t *pt = pd ? get_next_level(pd, pd_idx, true, flags) : NULL;

    if (pt) {
        pt[pt_idx] = (phys & PTE_ADDR_MASK) | flags | VMM_PRESENT;
        vmm_invlpg(virt);
        ok = true;
    }

    spinlock_release_irqrestore(&vmm_lock, irq_flags);
    return ok;
}

bool vmm_unmap_page(uint64_t *pml4, uint64_t virt) {
    if (pml4 == NULL) {
        pml4 = kernel_pml4;
    }

    size_t pml4_idx = (virt >> 39) & 0x1FF;
    size_t pdpt_idx = (virt >> 30) & 0x1FF;
    size_t pd_idx   = (virt >> 21) & 0x1FF;
    size_t pt_idx   = (virt >> 12) & 0x1FF;

    uint64_t irq_flags = spinlock_acquire_irqsave(&vmm_lock);
    bool ok = false;

    uint64_t *pdpt = get_next_level(pml4, pml4_idx, false, 0);
    uint64_t *pd = pdpt ? get_next_level(pdpt, pdpt_idx, false, 0) : NULL;
    uint64_t *pt = pd ? get_next_level(pd, pd_idx, false, 0) : NULL;

    if (pt && (pt[pt_idx] & VMM_PRESENT)) {
        pt[pt_idx] = 0;
        vmm_invlpg(virt);
        ok = true;
    }

    spinlock_release_irqrestore(&vmm_lock, irq_flags);
    return ok;
}

uint64_t vmm_get_phys(uint64_t *pml4, uint64_t virt) {
    if (pml4 == NULL) {
        pml4 = kernel_pml4;
    }

    size_t pml4_idx = (virt >> 39) & 0x1FF;
    size_t pdpt_idx = (virt >> 30) & 0x1FF;
    size_t pd_idx   = (virt >> 21) & 0x1FF;
    size_t pt_idx   = (virt >> 12) & 0x1FF;

    uint64_t irq_flags = spinlock_acquire_irqsave(&vmm_lock);
    uint64_t phys = 0;

    uint64_t *pdpt = get_next_level(pml4, pml4_idx, false, 0);
    uint64_t *pd = pdpt ? get_next_level(pdpt, pdpt_idx, false, 0) : NULL;
    uint64_t *pt = pd ? get_next_level(pd, pd_idx, false, 0) : NULL;

    if (pt && (pt[pt_idx] & VMM_PRESENT)) {
        phys = pt[pt_idx] & PTE_ADDR_MASK;
    }

    spinlock_release_irqrestore(&vmm_lock, irq_flags);
    return phys;
}

void vmm_invlpg(uint64_t virt) {
    asm volatile("invlpg [%0]" : : "r"(virt) : "memory");
}

void vmm_switch_pml4(uint64_t *pml4_phys) {
    asm volatile("mov cr3, %0" : : "r"(pml4_phys) : "memory");
}
