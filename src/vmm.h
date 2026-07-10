#ifndef VMM_H
#define VMM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Page Table Entry Flags
#define VMM_PRESENT     (1ULL << 0)
#define VMM_WRITABLE    (1ULL << 1)
#define VMM_USER        (1ULL << 2)
#define VMM_WRITE_THRU  (1ULL << 3)
#define VMM_CACHE_DIS   (1ULL << 4)
#define VMM_ACCESSED    (1ULL << 5)
#define VMM_DIRTY       (1ULL << 6)
#define VMM_HUGE        (1ULL << 7)
#define VMM_GLOBAL      (1ULL << 8)
#define VMM_NO_EXECUTE  (1ULL << 63)

#define PTE_ADDR_MASK   0x000FFFFFFFFFF000ULL

// Initialize Virtual Memory Manager
void vmm_init(void);

// Get kernel root PML4 page table (virtual address)
uint64_t *vmm_get_kernel_pml4(void);

// Map virtual address to physical address with specified flags
bool vmm_map_page(uint64_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags);

// Unmap a virtual page
bool vmm_unmap_page(uint64_t *pml4, uint64_t virt);

// Invalidate TLB entry for virtual address
void vmm_invlpg(uint64_t virt);

// Switch active PML4 by loading CR3 register
void vmm_switch_pml4(uint64_t *pml4_phys);

#endif // VMM_H
