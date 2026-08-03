/**
 * @file kstack.c
 * @brief Implementation of the guarded kernel stack allocator
 */

#include "kstack.h"
#include "task.h"
#include "pmm.h"
#include "vmm.h"
#include "spinlock.h"
#include <stdbool.h>

#define KSTACK_PAGES (KERNEL_STACK_SIZE / PAGE_SIZE)
// One slot = one unmapped guard page followed by the stack pages
#define KSTACK_SLOT_SIZE ((KSTACK_PAGES + 1) * PAGE_SIZE)

static bool slot_used[KSTACK_MAX_SLOTS];
static spinlock_t kstack_lock = SPINLOCK_RANKED("kstack", LOCK_RANK_KSTACK);

static uint64_t slot_stack_base(size_t slot) {
    return KSTACK_REGION_BASE + slot * KSTACK_SLOT_SIZE + PAGE_SIZE;
}

void *kstack_alloc(void) {
    uint64_t flags = spinlock_acquire_irqsave(&kstack_lock);
    size_t slot = KSTACK_MAX_SLOTS;
    for (size_t i = 0; i < KSTACK_MAX_SLOTS; i++) {
        if (!slot_used[i]) {
            slot_used[i] = true;
            slot = i;
            break;
        }
    }
    spinlock_release_irqrestore(&kstack_lock, flags);

    if (slot == KSTACK_MAX_SLOTS) return NULL;

    uint64_t base = slot_stack_base(slot);
    for (size_t p = 0; p < KSTACK_PAGES; p++) {
        void *frame = pmm_alloc_frame();
        if (!frame || !vmm_map_page(NULL, base + p * PAGE_SIZE, (uint64_t)frame,
                                    VMM_PRESENT | VMM_WRITABLE | VMM_NO_EXECUTE)) {
            if (frame) pmm_free_frame(frame);
            kstack_free((void *)base); // Undo the pages mapped so far
            return NULL;
        }
    }

    return (void *)base;
}

void kstack_free(void *stack_base) {
    uint64_t base = (uint64_t)stack_base;
    if (base < KSTACK_REGION_BASE + PAGE_SIZE) return;

    size_t slot = (base - KSTACK_REGION_BASE) / KSTACK_SLOT_SIZE;
    if (slot >= KSTACK_MAX_SLOTS || slot_stack_base(slot) != base) return;

    for (size_t p = 0; p < KSTACK_PAGES; p++) {
        uint64_t virt = base + p * PAGE_SIZE;
        uint64_t phys = vmm_get_phys(NULL, virt);
        if (phys) {
            vmm_unmap_page(NULL, virt);
            pmm_free_frame((void *)phys);
        }
    }

    uint64_t flags = spinlock_acquire_irqsave(&kstack_lock);
    slot_used[slot] = false;
    spinlock_release_irqrestore(&kstack_lock, flags);
}

int kstack_is_guard_page(uint64_t addr) {
    if (addr < KSTACK_REGION_BASE) return 0;
    uint64_t offset = addr - KSTACK_REGION_BASE;
    if (offset / KSTACK_SLOT_SIZE >= KSTACK_MAX_SLOTS) return 0;
    return (offset % KSTACK_SLOT_SIZE) < PAGE_SIZE;
}
