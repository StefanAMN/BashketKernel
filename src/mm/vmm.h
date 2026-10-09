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
#define VMM_COW         (1ULL << 9)  // Ours (CPU ignores bits 9-11): copy this page on first write
#define VMM_SHARED      (1ULL << 10) // Ours: shared memory; fork gives the child the same frame, writable
// In an entry that is NOT present (the CPU ignores every other bit of
// those): the page is on the swap disk, in the slot whose number is where a
// frame address would be. The permission bits are kept, for when it returns.
#define VMM_SWAPPED     (1ULL << 11)
// Ours, in a bit the CPU ignores (52..62 are free for software): the frame
// is a device's memory (the screen), not one the frame allocator owns or
// counts. Unmapping, destroying, fork and swap leave it alone: they must
// not give it back as free memory, nor count owners of it. Always set with
// VMM_SHARED, which swap and fork already treat as "not this process's".
#define VMM_DEVICE      (1ULL << 52)
#define VMM_NO_EXECUTE  (1ULL << 63) // NX bit (No Instruction Execution)

#define PTE_ADDR_MASK   0x000FFFFFFFFFF000ULL

// First address above user space. On x86_64 with 4-level paging the lower
// canonical half ends at 0x00007FFFFFFFFFFF.
#define USER_SPACE_END 0x0000800000000000ULL
#define DEVICE_REGION_BASE 0xFFFFC00000000000ULL  // Where device registers are mapped (vmm_map_device)
#define KERNEL_SPACE_START 0xFFFF800000000000ULL // The half every address space shares

/**
 * @brief Physical address of the kernel PML4, as loaded into CR3.
 */
uint64_t vmm_get_kernel_pml4_phys(void);

/**
 * @brief Convert a PML4 physical address (as stored in a task) into the
 * pointer form the vmm_* functions take.
 */
uint64_t *vmm_space_from_phys(uint64_t pml4_phys);

/**
 * @brief Create an address space: empty user half, kernel half shared.
 * @return PML4 pointer, or NULL if out of memory.
 */
uint64_t *vmm_create_address_space(void);

/**
 * @brief Free every user page and user page table of an address space, then
 * the PML4 itself. Must not be the address space currently loaded in CR3.
 */
void vmm_destroy_address_space(uint64_t *pml4);

/**
 * @brief Make `dst` a copy-on-write clone of the user half of `src` (fork).
 *
 * No page contents are copied. Both spaces end up mapping the same frames;
 * pages that were writable become read-only and marked VMM_COW in both, and
 * the first write to one is resolved by vmm_handle_cow_fault.
 *
 * @return false if memory ran out; the caller should destroy `dst`.
 */
bool vmm_clone_user_space(uint64_t *dst, uint64_t *src);

/**
 * @brief Resolve a write to a copy-on-write page by giving `pml4` its own
 *        writable copy (or just write access, if it is the last owner).
 * @return true if `virt` was a COW page and is now writable; false if the
 *         fault is a real protection violation or memory ran out.
 */
bool vmm_handle_cow_fault(uint64_t *pml4, uint64_t virt);

/**
 * @brief Read the raw page-table entry for a 4 KiB page (0 if unmapped).
 */
uint64_t vmm_get_pte(uint64_t *pml4, uint64_t virt);

/**
 * @brief Pages present in the user half of the address space `pml4`
 *        (what a process holds in memory: not those promised and never
 *        touched, nor those swapped out); in *shared, how many of them have
 *        another owner too (copy-on-write after fork, the page cache,
 *        shared memory). Walks only the tables that exist. Takes no lock:
 *        the caller must keep the address space from being destroyed
 *        meanwhile (the scheduler lock: see process.c and sched.c), and the
 *        counts are a snapshot while its owner runs.
 */
size_t vmm_count_user_pages(const uint64_t *pml4, size_t *shared);

/**
 * @brief Allocate a zeroed frame and map it at a user address.
 * @return Kernel (direct map) pointer to the new page, for filling it in
 *         without switching address space. NULL on failure.
 */
void *vmm_alloc_user_page(uint64_t *pml4, uint64_t virt, uint64_t flags);

/**
 * @brief Check that [addr, addr+len) lies in user space and that every page
 * is mapped user-accessible (and writable or copy-on-write, if `write`; the
 * kernel's own write to a COW page faults and is resolved like a user's). Syscalls must call
 * this before touching any pointer a user program passed in. With `write`, copy-on-write
 * pages in the range are given their private copy here, so the write that follows can not
 * fault (it usually happens under a lock); false if memory for a copy runs out.
 */
bool vmm_user_range_ok(uint64_t *pml4, uint64_t addr, size_t len, bool write);

/**
 * @brief Copy bytes into another address space through the direct map.
 * @return Number of bytes copied (short if a page is not mapped).
 */
size_t vmm_copy_to_space(uint64_t *pml4, uint64_t dest, const void *src, size_t len);

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
 * @brief Map a device's registers (memory that is not RAM, at a physical
 *        address the firmware chose) into the kernel and return where.
 *
 * They get a region of their own rather than their place in the direct
 * map: the bootloader's direct map may already cover the address with a
 * large page, and it maps everything as ordinary cached memory. Registers
 * need every access to reach the device, so these pages are mapped
 * uncached. The mapping is never removed.
 *
 * @return The address of `phys` itself (not of the start of its page), or
 *         NULL if a page table could not be allocated.
 */
void *vmm_map_device(uint64_t phys, size_t length);

// --- Swapped-out pages (swap.h) ---

static inline bool vmm_pte_swapped(uint64_t pte) {
    return !(pte & VMM_PRESENT) && (pte & VMM_SWAPPED);
}

static inline uint32_t vmm_pte_swap_slot(uint64_t pte) {
    return (uint32_t)((pte & PTE_ADDR_MASK) >> 12);
}

/** A range of user addresses, [start, end) */
typedef struct {
    uint64_t start;
    uint64_t end;
} vmm_range_t;

/**
 * @brief Choose a page of a user address space to swap out and take it
 *        away: its entry becomes a swapped entry for `slot`.
 *
 * Candidates are present user pages whose frame nobody else shares and
 * which touch none of the `keep_count` ranges in `keep` (the buffers of a
 * system call in progress; NULL and 0 for none). The search starts at
 * `*cursor` and wraps round; a page whose accessed bit is set has the bit
 * cleared and is passed over this time.
 *
 * @param virt, frame Receive the page's address and the frame it had, which
 *        the caller now holds the only reference to.
 * @return false if the address space has no page that may go.
 */
bool vmm_swap_pick(uint64_t *pml4, uint64_t *cursor, uint32_t slot, const vmm_range_t *keep, size_t keep_count,
                   uint64_t *virt, uint64_t *frame);

/**
 * @brief Turn the swapped entry for `virt` back into a present one that
 *        maps `frame`, with the permissions it had.
 */
bool vmm_swap_install(uint64_t *pml4, uint64_t virt, uint64_t frame);

/**
 * @brief If `virt` is swapped out, forget it: clear the entry and release
 *        its slot. For unmapping. Returns whether it was.
 */
bool vmm_swap_discard(uint64_t *pml4, uint64_t virt);

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
