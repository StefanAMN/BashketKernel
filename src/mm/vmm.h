/**
 * @file vmm.h
 * @brief Virtual Memory Manager (VMM) and x86_64 4-Level Paging Interface
 * 
 * Manages 4-level page tables (PML4 -> PDPT -> PD -> PT) and virtual address mapping.
 */

#ifndef VMM_H
#define VMM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// x86_64 Page Table Entry Bits per Intel SDM Volume 3
#define VMM_PRESENT     (1ULL << 0)  // Page is present in physical memory
#define VMM_WRITABLE    (1ULL << 1)  // Page is read/write
#define VMM_USER        (1ULL << 2)  // User mode (Ring 3) accessible
#define VMM_WRITE_THRU  (1ULL << 3)  // Write-through caching
#define VMM_CACHE_DIS   (1ULL << 4)  // Cache disable
#define VMM_ACCESSED    (1ULL << 5)  // Set by CPU when read/written
#define VMM_DIRTY       (1ULL << 6)  // Set by CPU when written
#define VMM_HUGE        (1ULL << 7)  // Huge Page (2MB or 1GB)
#define VMM_GLOBAL      (1ULL << 8)  // Global translation (not invalidated on CR3 write)
#define VMM_NO_EXECUTE  (1ULL << 63) // NX bit (No Instruction Execution)

#define PTE_ADDR_MASK   0x000FFFFFFFFFF000ULL

/**
 * @brief Initialize Virtual Memory Manager from current CR3 page table.
 */
void vmm_init(void);

/**
 * @brief Get active Kernel PML4 root table virtual address.
 * @return Virtual address pointer to PML4 table.
 */
uint64_t *vmm_get_kernel_pml4(void);

/**
 * @brief Map a virtual page to a physical frame with specified permission flags.
 * @param pml4 Pointer to PML4 table (pass NULL to use active kernel PML4).
 * @param virt 64-bit Virtual page address.
 * @param phys 64-bit Physical frame address.
 * @param flags Permission flags (VMM_PRESENT, VMM_WRITABLE, VMM_USER, etc.).
 * @return true on success, false on failure.
 */
bool vmm_map_page(uint64_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags);

/**
 * @brief Unmap a virtual page address.
 * @param pml4 Pointer to PML4 table (pass NULL for kernel PML4).
 * @param virt 64-bit Virtual page address.
 * @return true if page was unmapped, false if not mapped.
 */
bool vmm_unmap_page(uint64_t *pml4, uint64_t virt);

/**
 * @brief Look up the physical frame a 4 KiB virtual page is mapped to.
 * @param pml4 Pointer to PML4 table (pass NULL for kernel PML4).
 * @param virt 64-bit Virtual page address.
 * @return Physical frame address, or 0 if the page is not mapped.
 */
uint64_t vmm_get_phys(uint64_t *pml4, uint64_t virt);

/**
 * @brief Invalidate TLB cache entry for virtual address using `invlpg`.
 * @param virt Virtual address to invalidate.
 */
void vmm_invlpg(uint64_t virt);

/**
 * @brief Load new PML4 physical address into CR3 register.
 * @param pml4_phys Physical address of PML4 table.
 */
void vmm_switch_pml4(uint64_t *pml4_phys);

#endif // VMM_H
