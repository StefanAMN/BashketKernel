/**
 * @file vmm.c
 * @brief Implementation of Virtual Memory Manager and 4-Level Paging
 */

#include "vmm.h"
#include "smp.h"
#include "pmm.h"
#include "kprintf.h"
#include "string_util.h"
#include "spinlock.h"
#include "cow.h"
#include "swap.h"

static uint64_t *kernel_pml4 = NULL;

// Protects page-table edits. Lock order: vmm_lock, then the PMM lock.
static spinlock_t vmm_lock = SPINLOCK_RANKED("vmm", LOCK_RANK_VMM);

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
        if (!(entry & VMM_PRESENT)) {
            // A page that is on the swap disk: its slot is free again
            if (level == 1 && vmm_pte_swapped(entry)) swap_slot_unref(vmm_pte_swap_slot(entry));
            continue;
        }

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

// Store an entry exactly as given (it need not be a present one), making
// the tables above it as needed
static bool set_raw_entry(uint64_t *pml4, uint64_t virt, uint64_t entry) {
    uint64_t irq_flags = spinlock_acquire_irqsave(&vmm_lock);
    uint64_t *pdpt = get_next_level(pml4, (virt >> 39) & 0x1FF, true, VMM_USER);
    uint64_t *pd = pdpt ? get_next_level(pdpt, (virt >> 30) & 0x1FF, true, VMM_USER) : NULL;
    uint64_t *pt = pd ? get_next_level(pd, (virt >> 21) & 0x1FF, true, VMM_USER) : NULL;
    if (pt) pt[(virt >> 12) & 0x1FF] = entry;
    spinlock_release_irqrestore(&vmm_lock, irq_flags);
    return pt != NULL;
}

// --- Swapped-out pages -------------------------------------------------------------

// Look in [from, to) for a page that may be swapped out; on finding one,
// replace its entry. Caller holds vmm_lock.
static bool swap_scan(uint64_t *pml4, uint64_t from, uint64_t to, uint32_t slot, uint64_t *virt_out, uint64_t *frame_out) {
    uint64_t hhdm = pmm_get_hhdm_offset();
    uint64_t v = from & ~(uint64_t)(PAGE_SIZE - 1);

    while (v < to) {
        // Whole stretches with no tables behind them are stepped over at once
        uint64_t e4 = pml4[(v >> 39) & 0x1FF];
        if (!(e4 & VMM_PRESENT)) { v = (v | ((1ULL << 39) - 1)) + 1; continue; }
        uint64_t e3 = ((uint64_t *)((e4 & PTE_ADDR_MASK) + hhdm))[(v >> 30) & 0x1FF];
        if (!(e3 & VMM_PRESENT) || (e3 & VMM_HUGE)) { v = (v | ((1ULL << 30) - 1)) + 1; continue; }
        uint64_t e2 = ((uint64_t *)((e3 & PTE_ADDR_MASK) + hhdm))[(v >> 21) & 0x1FF];
        if (!(e2 & VMM_PRESENT) || (e2 & VMM_HUGE)) { v = (v | ((1ULL << 21) - 1)) + 1; continue; }
        uint64_t *pt = (uint64_t *)((e2 & PTE_ADDR_MASK) + hhdm);

        do {
            uint64_t *pte = &pt[(v >> 12) & 0x1FF];
            uint64_t entry = *pte;
            uint64_t frame = entry & PTE_ADDR_MASK;
            // Only a page this address space has to itself: shared memory,
            // and frames still shared after a fork, stay
            if ((entry & VMM_PRESENT) && (entry & VMM_USER) && !(entry & VMM_SHARED) &&
                pmm_get_frame_ref((void *)frame) == 1) {
                if (entry & VMM_ACCESSED) {
                    // Used since the hand was last here: a second chance.
                    // (The CPU sets the bit again only if it has to look at
                    // this entry again, hence the invlpg.)
                    *pte = entry & ~VMM_ACCESSED;
                    vmm_invlpg(v);
                } else {
                    *pte = (entry & ~(VMM_PRESENT | PTE_ADDR_MASK | VMM_DIRTY)) | VMM_SWAPPED | ((uint64_t)slot << 12);
                    vmm_invlpg(v);
                    *virt_out = v;
                    *frame_out = frame;
                    return true;
                }
            }
            v += PAGE_SIZE;
        } while ((v & ((1ULL << 21) - 1)) != 0 && v < to);
    }
    return false;
}

bool vmm_swap_pick(uint64_t *pml4, uint64_t *cursor, uint32_t slot, uint64_t *virt, uint64_t *frame) {
    if (!pml4 || pml4 == kernel_pml4 || !cursor) return false;
    uint64_t start = *cursor < USER_SPACE_END ? *cursor : 0;

    // From the hand to the end, then all the way round, then once more:
    // the first time round may only have cleared accessed bits
    uint64_t irq_flags = spinlock_acquire_irqsave(&vmm_lock);
    bool found = swap_scan(pml4, start, USER_SPACE_END, slot, virt, frame) ||
                 swap_scan(pml4, 0, USER_SPACE_END, slot, virt, frame) ||
                 swap_scan(pml4, 0, USER_SPACE_END, slot, virt, frame);
    if (found) *cursor = *virt + PAGE_SIZE;
    spinlock_release_irqrestore(&vmm_lock, irq_flags);
    return found;
}

bool vmm_swap_install(uint64_t *pml4, uint64_t virt, uint64_t frame) {
    if (!pml4 || virt >= USER_SPACE_END) return false;
    virt &= ~(uint64_t)(PAGE_SIZE - 1);

    uint64_t irq_flags = spinlock_acquire_irqsave(&vmm_lock);
    uint64_t *pte = walk_to_pte(pml4, virt);
    bool ok = pte && vmm_pte_swapped(*pte);
    if (ok) {
        // Marked accessed: it is about to be used, and must not be the
        // first to go out again
        *pte = (*pte & ~(PTE_ADDR_MASK | VMM_SWAPPED)) | (frame & PTE_ADDR_MASK) | VMM_PRESENT | VMM_ACCESSED;
        vmm_invlpg(virt);
    }
    spinlock_release_irqrestore(&vmm_lock, irq_flags);
    return ok;
}

bool vmm_swap_discard(uint64_t *pml4, uint64_t virt) {
    if (!pml4 || virt >= USER_SPACE_END) return false;
    uint32_t slot = 0;

    uint64_t irq_flags = spinlock_acquire_irqsave(&vmm_lock);
    uint64_t *pte = walk_to_pte(pml4, virt);
    bool was = pte && vmm_pte_swapped(*pte);
    if (was) {
        slot = vmm_pte_swap_slot(*pte);
        *pte = 0;
    }
    spinlock_release_irqrestore(&vmm_lock, irq_flags);
    if (was) swap_slot_unref(slot);
    return was;
}

bool vmm_clone_user_space(uint64_t *dst, uint64_t *src) {
    if (!dst || !src) return false;
    uint64_t hhdm = pmm_get_hhdm_offset();
    bool ok = true;

    for (size_t i4 = 0; i4 < 256 && ok; i4++) {
        if (!(src[i4] & VMM_PRESENT)) continue;
        uint64_t *pdpt = (uint64_t *)((src[i4] & PTE_ADDR_MASK) + hhdm);

        for (size_t i3 = 0; i3 < 512 && ok; i3++) {
            if (!(pdpt[i3] & VMM_PRESENT) || (pdpt[i3] & VMM_HUGE)) continue;
            uint64_t *pd = (uint64_t *)((pdpt[i3] & PTE_ADDR_MASK) + hhdm);

            for (size_t i2 = 0; i2 < 512 && ok; i2++) {
                if (!(pd[i2] & VMM_PRESENT) || (pd[i2] & VMM_HUGE)) continue;
                uint64_t *pt = (uint64_t *)((pd[i2] & PTE_ADDR_MASK) + hhdm);

                for (size_t i1 = 0; i1 < 512; i1++) {
                    uint64_t pte = pt[i1];
                    if (vmm_pte_swapped(pte)) {
                        // On the swap disk: the child gets the same entry,
                        // and the slot one more owner. Each reads its own
                        // copy back, so nothing needs marking copy-on-write.
                        uint64_t where = ((uint64_t)i4 << 39) | ((uint64_t)i3 << 30) |
                                         ((uint64_t)i2 << 21) | ((uint64_t)i1 << 12);
                        if (!set_raw_entry(dst, where, pte)) {
                            ok = false;
                            break;
                        }
                        swap_slot_ref(vmm_pte_swap_slot(pte));
                        continue;
                    }
                    if (!(pte & VMM_PRESENT)) continue;

                    // A writable page becomes read-only + COW in the parent
                    // too: whichever side writes first gets the copy.
                    // Read-only pages (program text) are simply shared.
                    // Shared memory is the exception: it stays writable and
                    // both processes keep seeing each other's writes.
                    if ((pte & VMM_WRITABLE) && !(pte & VMM_SHARED)) {
                        pte = (pte & ~VMM_WRITABLE) | VMM_COW;
                    }

                    uint64_t virt = ((uint64_t)i4 << 39) | ((uint64_t)i3 << 30) |
                                    ((uint64_t)i2 << 21) | ((uint64_t)i1 << 12);
                    uint64_t phys = pte & PTE_ADDR_MASK;
                    uint64_t flags = pte & (VMM_USER | VMM_COW | VMM_NO_EXECUTE | VMM_SHARED | VMM_WRITABLE);

                    // Count the new owner before it exists, so a failure
                    // below is undone by destroying dst like any other space.
                    pmm_ref_frame((void *)phys);
                    if (!vmm_map_page(dst, virt, phys, flags | VMM_PRESENT)) {
                        pmm_unref_frame((void *)phys);
                        ok = false;
                        break;
                    }
                    pt[i1] = pte;
                }
            }
        }
    }

    // The parent's page tables changed under a live TLB: writes it could do
    // a moment ago must now fault. Reloading CR3 drops the stale entries.
    uint64_t cr3;
    asm volatile("mov %0, cr3" : "=r"(cr3));
    if ((cr3 & PTE_ADDR_MASK) == (uint64_t)src - hhdm) {
        asm volatile("mov cr3, %0" : : "r"(cr3) : "memory");
    }

    return ok;
}

bool vmm_handle_cow_fault(uint64_t *pml4, uint64_t virt) {
    if (!pml4 || virt >= USER_SPACE_END) return false;
    virt &= ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t hhdm = pmm_get_hhdm_offset();

    uint64_t irq_flags = spinlock_acquire_irqsave(&vmm_lock);
    bool resolved = false;

    uint64_t *pte = walk_to_pte(pml4, virt);
    if (pte && (*pte & VMM_PRESENT) && (*pte & VMM_COW)) {
        uint64_t old_phys = *pte & PTE_ADDR_MASK;
        uint64_t flags = (*pte & ~(PTE_ADDR_MASK | VMM_COW)) | VMM_WRITABLE;

        if (pmm_get_frame_ref((void *)old_phys) == 1) {
            // Every other owner has gone or already copied: just take it
            *pte = old_phys | flags;
            resolved = true;
        } else {
            void *copy = pmm_alloc_frame();
            if (copy) {
                memcpy((void *)((uint64_t)copy + hhdm), (void *)(old_phys + hhdm), PAGE_SIZE);
                *pte = (uint64_t)copy | flags;
                pmm_unref_frame((void *)old_phys);
                resolved = true;
            }
        }

        if (resolved) vmm_invlpg(virt);
    }

    spinlock_release_irqrestore(&vmm_lock, irq_flags);
    return resolved;
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
        if (write && !(pte & (VMM_WRITABLE | VMM_COW))) return false;

        // The kernel is about to write here, often while holding a lock
        // (a pipe's, say), where a fault that could not be resolved would
        // have to kill the task lock and all. Give the page its private
        // copy now instead; running out of memory is an ordinary error here.
        if (write && (pte & VMM_COW) && !vmm_handle_cow_fault(pml4, page)) return false;
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

void *vmm_map_device(uint64_t phys, size_t length) {
    static uint64_t next_free = DEVICE_REGION_BASE;
    if (length == 0) return NULL;

    uint64_t first = phys & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t pages = (phys + length - first + PAGE_SIZE - 1) / PAGE_SIZE;

    // Take the addresses first (one page more than needed: an unmapped page
    // between one device's registers and the next), then fill them in
    uint64_t virt = __atomic_fetch_add(&next_free, (pages + 1) * PAGE_SIZE, __ATOMIC_SEQ_CST);
    for (uint64_t i = 0; i < pages; i++) {
        // PCD and PWT both set selects "uncacheable" in the default page
        // attribute table (Intel SDM Vol. 3A, 12.12.3, table 12-11)
        if (!vmm_map_page(NULL, virt + i * PAGE_SIZE, first + i * PAGE_SIZE,
                          VMM_PRESENT | VMM_WRITABLE | VMM_CACHE_DIS | VMM_WRITE_THRU | VMM_NO_EXECUTE)) return NULL;
    }
    return (void *)(virt + (phys - first));
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

    // The kernel half is mapped on every CPU, and any of them may have this
    // translation cached. (A user address space is only ever live on the
    // one CPU running its process, and a CPU drops those translations when
    // it switches to another address space.) Done outside the lock: the
    // other CPUs must be able to take it while we wait for them.
    if (ok && virt >= KERNEL_SPACE_START) smp_tlb_shootdown();
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
