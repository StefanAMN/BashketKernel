/**
 * @file cow.c
 * @brief Implementation of physical frame sharing counts
 */

#include "cow.h"
#include "pmm.h"
#include "kprintf.h"
#include "string_util.h"
#include "spinlock.h"

// extra_owners[frame] = owners beyond the first. Zero for almost every frame,
// so the table needs no setup when a frame is allocated.
static uint16_t *extra_owners = NULL;
static size_t tracked_frames = 0;
static spinlock_t cow_lock = SPINLOCK_INIT;

void cow_init(void) {
    tracked_frames = pmm_get_total_frames();
    size_t bytes = tracked_frames * sizeof(uint16_t);
    size_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    // Too large for the kernel heap on big machines: take whole frames
    void *phys = pmm_alloc_frames(pages);
    if (!phys) panic("COW: cannot allocate the frame sharing table");

    extra_owners = (uint16_t *)((uint64_t)phys + pmm_get_hhdm_offset());
    memset(extra_owners, 0, pages * PAGE_SIZE);
    kprintf("COW Init: Sharing table covers %zu frames (%zu KB)\n", tracked_frames, bytes / 1024);
}

void pmm_ref_frame(void *phys_frame) {
    uint64_t frame = (uint64_t)phys_frame / PAGE_SIZE;
    if (!extra_owners || frame >= tracked_frames) return;

    uint64_t flags = spinlock_acquire_irqsave(&cow_lock);
    extra_owners[frame]++;
    spinlock_release_irqrestore(&cow_lock, flags);
}

void pmm_unref_frame(void *phys_frame) {
    if (!phys_frame) return;
    uint64_t frame = (uint64_t)phys_frame / PAGE_SIZE;

    bool last_owner = true;
    if (extra_owners && frame < tracked_frames) {
        uint64_t flags = spinlock_acquire_irqsave(&cow_lock);
        if (extra_owners[frame] > 0) {
            extra_owners[frame]--;
            last_owner = false;
        }
        spinlock_release_irqrestore(&cow_lock, flags);
    }

    if (last_owner) {
        pmm_free_frame(phys_frame);
    }
}

uint32_t pmm_get_frame_ref(void *phys_frame) {
    uint64_t frame = (uint64_t)phys_frame / PAGE_SIZE;
    if (!extra_owners || frame >= tracked_frames) return 1;

    uint64_t flags = spinlock_acquire_irqsave(&cow_lock);
    uint32_t owners = (uint32_t)extra_owners[frame] + 1;
    spinlock_release_irqrestore(&cow_lock, flags);
    return owners;
}
