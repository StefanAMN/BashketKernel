/**
 * @file mmap.c
 * @brief Anonymous memory mappings and program break for user address spaces
 */

#include "mmap.h"
#include "vmm.h"
#include "pmm.h"
#include "cow.h"
#include "string_util.h"

static uint64_t page_align_up(uint64_t value) {
    return (value + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
}

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
            vmm_unmap_page(pml4, page);
            pmm_unref_frame((void *)phys);
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

uint64_t user_mmap_anon(uint64_t *pml4, uint64_t *next_addr, size_t length, int prot) {
    return user_mmap_anon_flags(pml4, next_addr, length, prot, false);
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

uint64_t user_set_brk(uint64_t *pml4, uint64_t brk_start, uint64_t *brk_end, uint64_t new_brk) {
    if (!pml4 || !brk_end) return 0;

    // Out-of-range requests leave the break where it is; the caller sees the
    // unchanged value and knows the request failed (Linux brk semantics).
    if (new_brk < brk_start || new_brk > brk_start + USER_BRK_MAX) return *brk_end;

    uint64_t old_top = page_align_up(*brk_end);
    uint64_t new_top = page_align_up(new_brk);

    if (new_top > old_top) {
        if (user_map_range(pml4, old_top, new_top - old_top, PROT_READ | PROT_WRITE) != 0) {
            return *brk_end;
        }
    } else if (new_top < old_top) {
        user_unmap_range(pml4, new_top, old_top - new_top);
    }

    *brk_end = new_brk;
    return new_brk;
}
