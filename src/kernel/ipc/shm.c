/**
 * @file shm.c
 * @brief Shared memory segments (see shm.h)
 */

#include "shm.h"
#include "pmm.h"
#include "vmm.h"
#include "cow.h"
#include "mmap.h"
#include "heap.h"
#include "kstring.h"
#include "spinlock.h"
#include "kerrno.h"
#include "auth.h"

typedef struct {
    bool used;
    uint32_t key;
    uint32_t owner;     // The user who created it: only they (and root) may attach or remove it
    size_t pages;
    uint64_t *frames;   // Physical address of each page. The table holds one reference to each.
} shm_segment_t;

static shm_segment_t segments[SHM_MAX_SEGMENTS];
static spinlock_t shm_lock = SPINLOCK_RANKED("shm", LOCK_RANK_IPC);

size_t shm_count(void) {
    size_t count = 0;
    for (int i = 0; i < SHM_MAX_SEGMENTS; i++) count += segments[i].used ? 1 : 0;
    return count;
}

int shm_get(uint32_t key, size_t size) {
    size_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (size == 0 || pages == 0 || pages > SHM_MAX_PAGES) return -EINVAL;

    // Allocate before looking, so that the table is searched and filled in
    // one step under the table's lock (allocation takes locks of its own)
    uint64_t *frames = kmalloc(pages * sizeof(uint64_t));
    if (!frames) return -ENOMEM;
    for (size_t i = 0; i < pages; i++) {
        frames[i] = (uint64_t)pmm_alloc_frame();
        if (!frames[i]) {
            while (i > 0) pmm_free_frame((void *)frames[--i]);
            kfree(frames);
            return -ENOMEM;
        }
        memset((void *)(frames[i] + pmm_get_hhdm_offset()), 0, PAGE_SIZE); // Never hand out old contents
    }

    int result = -ENOSPC;
    bool used_new_frames = false;

    uint64_t flags = spinlock_acquire_irqsave(&shm_lock);
    int free_slot = -1;
    for (int i = 0; i < SHM_MAX_SEGMENTS; i++) {
        if (segments[i].used && segments[i].key == key) {
            result = (pages <= segments[i].pages) ? i : -EINVAL; // It exists; is it big enough?
            free_slot = -1;
            goto done;
        }
        if (!segments[i].used && free_slot < 0) free_slot = i;
    }
    if (free_slot >= 0) {
        segments[free_slot].used = true;
        segments[free_slot].key = key;
        segments[free_slot].pages = pages;
        segments[free_slot].frames = frames;
        segments[free_slot].owner = auth_current_uid();
        used_new_frames = true;
        result = free_slot;
    }
done:
    spinlock_release_irqrestore(&shm_lock, flags);

    if (!used_new_frames) {
        for (size_t i = 0; i < pages; i++) pmm_free_frame((void *)frames[i]);
        kfree(frames);
    }
    return result;
}

bool shm_count_valid(int id) {
    if (id < 0 || id >= SHM_MAX_SEGMENTS) return false;
    uint64_t flags = spinlock_acquire_irqsave(&shm_lock);
    bool used = segments[id].used;
    spinlock_release_irqrestore(&shm_lock, flags);
    return used;
}

bool shm_may_use(int id, uint32_t uid) {
    if (id < 0 || id >= SHM_MAX_SEGMENTS) return false;
    uint64_t flags = spinlock_acquire_irqsave(&shm_lock);
    bool ok = segments[id].used && (uid == UID_ROOT || uid == segments[id].owner);
    spinlock_release_irqrestore(&shm_lock, flags);
    return ok;
}

uint64_t shm_attach(int id, uint64_t *pml4, uint64_t *mmap_next) {
    if (id < 0 || id >= SHM_MAX_SEGMENTS || !pml4 || !mmap_next) return 0;

    // Take our own references to the frames first: after that the segment
    // can be removed by someone else at any moment without harm
    uint64_t frames[SHM_MAX_PAGES];
    size_t pages = 0;
    uint64_t flags = spinlock_acquire_irqsave(&shm_lock);
    if (segments[id].used) {
        pages = segments[id].pages;
        for (size_t i = 0; i < pages; i++) {
            frames[i] = segments[id].frames[i];
            pmm_ref_frame((void *)frames[i]);
        }
    }
    spinlock_release_irqrestore(&shm_lock, flags);
    if (pages == 0) return 0;

    uint64_t addr = *mmap_next;
    uint64_t size = pages * PAGE_SIZE;
    size_t mapped = 0;
    if (addr < USER_MMAP_END && size <= USER_MMAP_END - addr) {
        for (; mapped < pages; mapped++) {
            if (!vmm_map_page(pml4, addr + mapped * PAGE_SIZE, frames[mapped],
                              VMM_PRESENT | VMM_USER | VMM_WRITABLE | VMM_NO_EXECUTE | VMM_SHARED)) break;
        }
    }

    if (mapped < pages) {
        // Could not map all of it: undo, and give back every reference taken
        for (size_t i = 0; i < mapped; i++) vmm_unmap_page(pml4, addr + i * PAGE_SIZE);
        for (size_t i = 0; i < pages; i++) pmm_unref_frame((void *)frames[i]);
        return 0;
    }

    *mmap_next = addr + size;
    return addr;
}

int shm_remove(int id) {
    if (id < 0 || id >= SHM_MAX_SEGMENTS) return -EINVAL;

    uint64_t flags = spinlock_acquire_irqsave(&shm_lock);
    bool existed = segments[id].used;
    uint64_t *frames = segments[id].frames;
    size_t pages = segments[id].pages;
    segments[id].used = false;
    segments[id].frames = NULL;
    segments[id].pages = 0;
    spinlock_release_irqrestore(&shm_lock, flags);
    if (!existed) return -EINVAL;

    // Drop the table's reference. A frame still mapped somewhere lives on
    // until that mapping goes; the others are freed now.
    for (size_t i = 0; i < pages; i++) pmm_unref_frame((void *)frames[i]);
    kfree(frames);
    return 0;
}
