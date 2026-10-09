/**
 * @file mmap.c
 * @brief Anonymous memory mappings and program break for user address spaces
 */

#include "mmap.h"
#include "vmm.h"
#include "pmm.h"
#include "cow.h"
#include "kstring.h"

// The page-table bits for memory a program may use with protection `prot`
// (PROT_READ, PROT_WRITE, PROT_EXEC): always present and reachable from
// ring 3; writable only if asked; never executable unless asked (NX).
static uint64_t prot_to_pte_flags(int prot) {
    uint64_t flags = VMM_PRESENT | VMM_USER;
    if (prot & PROT_WRITE) flags |= VMM_WRITABLE;
    if (!(prot & PROT_EXEC)) flags |= VMM_NO_EXECUTE;
    return flags;
}

int user_unmap_range(uint64_t *pml4, uint64_t addr, size_t length) {
    if (!pml4 || length == 0 || (addr & (PAGE_SIZE - 1))) return -1;
    if (addr >= USER_SPACE_END || length > USER_SPACE_END - addr) return -1;

    uint64_t end = addr + page_align_up(length);
    for (uint64_t page = addr; page < end; page += PAGE_SIZE) {
        uint64_t phys = vmm_get_phys(pml4, page);
        if (phys) {
            bool device = (vmm_get_pte(pml4, page) & VMM_DEVICE) != 0;
            vmm_unmap_page(pml4, page);
            if (!device) pmm_unref_frame((void *)phys);   // (A device's memory is not the allocator's)
        } else {
            vmm_swap_discard(pml4, page); // On the swap disk: give the slot back
        }
    }
    return 0;
}

int user_map_range(uint64_t *pml4, uint64_t addr, size_t length, int prot) {
    if (!pml4 || length == 0 || (addr & (PAGE_SIZE - 1))) return -1;
    if (addr < PAGE_SIZE || addr >= USER_SPACE_END || length > USER_SPACE_END - addr) return -1;

    uint64_t flags = prot_to_pte_flags(prot);
    uint64_t end = addr + page_align_up(length);

    for (uint64_t page = addr; page < end; page += PAGE_SIZE) {
        if (vmm_get_pte(pml4, page) & VMM_PRESENT) continue; // Already backed

        if (!vmm_alloc_user_page(pml4, page, flags)) {
            // Out of memory: give back what this call mapped so far
            if (page > addr) user_unmap_range(pml4, addr, page - addr);
            return -1;
        }
    }
    return 0;
}

uint64_t user_mmap_anon_flags(uint64_t *pml4, uint64_t *next_addr, size_t length, int prot, bool shared) {
    if (!pml4 || !next_addr || length == 0) return 0;

    uint64_t size = page_align_up(length);
    if (size < length) return 0; // Overflow

    uint64_t addr = *next_addr;
    if (addr >= USER_MMAP_END || size > USER_MMAP_END - addr) return 0;

    if (user_map_range(pml4, addr, size, prot) != 0) return 0;

    // Shared: mark every page, so that fork hands the same frames to the
    // child instead of making them copy-on-write
    if (shared) {
        for (uint64_t page = addr; page < addr + size; page += PAGE_SIZE) {
            uint64_t pte = vmm_get_pte(pml4, page);
            vmm_map_page(pml4, page, pte & PTE_ADDR_MASK,
                         (pte & (VMM_PRESENT | VMM_USER | VMM_WRITABLE | VMM_NO_EXECUTE)) | VMM_SHARED);
        }
    }

    *next_addr = addr + size;
    return addr;
}

