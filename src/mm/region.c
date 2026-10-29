/**
 * @file region.c
 * @brief Memory a process has been promised but not yet given (see region.h)
 *
 * A process's regions, heap limits and count of promised pages are changed
 * under its map_mutex and mem_lock both, and read by page faults under
 * mem_lock alone (task.h says why two). A task's parent sets them up before
 * it first runs, and whoever frees it does so when it is dead, with no lock:
 * nobody else can see them then. The system-wide total is atomic.
 */

#include "region.h"
#include "task.h"
#include "mmap.h"
#include "vmm.h"
#include "pmm.h"
#include "cow.h"
#include "vfs.h"
#include "process.h"
#include "kstring.h"
#include "kerrno.h"
#include "swap.h"
#include "pcache.h"
#include "heap.h"
#include "sched.h"
#include "timer.h"

static size_t promised_total = 0;

size_t region_promised_pages(void) {
    return __atomic_load_n(&promised_total, __ATOMIC_RELAXED);
}

// The page tables of `task`'s address space, at their kernel address
static uint64_t *space_of(const struct task *task) {
    return vmm_space_from_phys((uint64_t)task->pml4_phys);
}

// Count `pages` more as promised to `task` and not yet given a frame: in its
// own total and in the system's (which limits what can be promised)
static void charge(struct task *task, size_t pages) {
    task->proc->promised_pages += pages;
    __atomic_add_fetch(&promised_total, pages, __ATOMIC_RELAXED);
}

// The reverse of charge: `pages` of the task's promise have been kept (given
// a frame) or withdrawn (unmapped). Never takes more than the task has.
static void uncharge(struct task *task, size_t pages) {
    if (pages > task->proc->promised_pages) pages = task->proc->promised_pages;
    task->proc->promised_pages -= pages;
    __atomic_sub_fetch(&promised_total, pages, __ATOMIC_RELAXED);
}

// Whether `pages` more could be promised with what is free now (and on the
// swap disk), less what is promised already and the reserve fork insists on
static bool fits(size_t pages) {
    size_t free_frames = pmm_get_free_frames();
    size_t spoken_for = region_promised_pages() + PROCESS_FORK_RESERVE_FRAMES;
    // (With a swap disk, pages can also be kept there)
    free_frames += swap_free_pages();
    return free_frames > spoken_for && pages <= free_frames - spoken_for;
}

// May this many more pages be promised? If not with what is free, then
// after the page cache has given back what is mapped nowhere (as good as
// free, but only once it is asked).
static bool can_promise(size_t pages) {
    if (fits(pages)) return true;
    pcache_reclaim(pages + PROCESS_FORK_RESERVE_FRAMES);
    return fits(pages);
}

static mem_region_t *free_slot(struct task *task);

// Pages of [start, end) that have no frame yet
static size_t untouched_pages(const struct task *task, uint64_t start, uint64_t end) {
    size_t count = 0;
    uint64_t *space = space_of(task);
    for (uint64_t page = start; page < end; page += PAGE_SIZE) {
        uint64_t pte = vmm_get_pte(space, page);
        if (!(pte & VMM_PRESENT) && !vmm_pte_swapped(pte)) count++; // (Swapped out is not untouched)
    }
    return count;
}

// A region's file must not be removed while the region exists: it counts as
// open, like a file descriptor or a working directory. A region of the
// program itself also keeps the file from being written (exec_count); a
// writable shared mapping counts as a writer, which keeps the file from
// being run (write_count).
static void pin(const mem_region_t *region, int by) {
    if (!region->node) return;
    vfs_lock();
    region->node->open_count += by;
    if (region->program) region->node->exec_count += by;
    if (region->shared && (region->prot & PROT_WRITE)) region->node->write_count += by;
    vfs_unlock();
}

// A mapping of a device (the screen) holds it as an open descriptor would
// (its opened and closed operations: the console takes the screen back only
// when every descriptor and every mapping is gone). +1 when a region of it
// is made (it may refuse: a negative error), -1 when one goes.
static int hold_device(const mem_region_t *r, int by) {
    if (!r->device || !r->node->ops) return 0;
    bool writable = (r->prot & PROT_WRITE) != 0;
    if (by > 0) return r->node->ops->opened ? r->node->ops->opened(r->node, writable) : 0;
    if (r->node->ops->closed) r->node->ops->closed(r->node, writable);
    return 0;
}

// The pages of `r`'s file that [start, end) (inside `r`) maps: indices
// [*first, *end_index)
static void file_pages(const mem_region_t *r, uint64_t start, uint64_t end, uint64_t *first, uint64_t *end_index) {
    *first = (r->file_offset + (start - r->start)) / PAGE_SIZE;
    *end_index = *first + (end - start) / PAGE_SIZE;
}

void region_hold_program(struct vnode *node, int by) {
    mem_region_t region = { .node = node, .program = true };
    pin(&region, by);
}

bool region_reserve(size_t pages) {
    if (!can_promise(pages)) return false;
    __atomic_add_fetch(&promised_total, pages, __ATOMIC_RELAXED);
    return true;
}

void region_unreserve(size_t pages) {
    __atomic_sub_fetch(&promised_total, pages, __ATOMIC_RELAXED);
}

void region_adopt(struct task *task, const mem_region_t *regions, uint32_t count, size_t pages) {
    for (uint32_t i = 0; i < count; i++) {
        mem_region_t *slot = free_slot(task);
        if (slot) *slot = regions[i];      // (Always: the caller made room)
    }
    task->proc->promised_pages += pages;         // Already in the system's total (region_reserve)
}

// An unused entry of the task's table of regions, or NULL if all are in use
static mem_region_t *free_slot(struct task *task) {
    for (int i = 0; i < TASK_MAX_REGIONS; i++) {
        if (task->proc->regions[i].end == 0) return &task->proc->regions[i];
    }
    return NULL;
}

// region_map, with the caller holding the process's map_mutex: nobody else
// changes the table, so a free slot stays free. The new entry appears, whole,
// under mem_lock; everything that touches the file comes before that.
static uint64_t map_locked(struct task *task, uint64_t size, int prot, struct vnode *node, uint64_t offset, bool shared, int *error) {
    uint64_t addr = task->proc->mmap_next;
    *error = -ENOMEM;
    if (addr >= USER_MMAP_END || size > USER_MMAP_END - addr) return 0;

    mem_region_t *slot = free_slot(task);
    if (!slot) return 0;
    bool device = node && node->ops && node->ops->map_page;
    if (!device && !can_promise(size / PAGE_SIZE)) return 0;   // (A device's pages are not memory to promise)

    mem_region_t region = { .start = addr, .end = addr + size, .prot = prot, .node = node, .file_offset = offset,
                            .program = false, .shared = shared && node && !device, .device = device };
    pin(&region, +1);
    int held = hold_device(&region, +1);
    if (held < 0) {
        pin(&region, -1);
        *error = held;
        return 0;
    }

    mutex_lock(&task->proc->mem_lock);
    *slot = region;
    if (!device) charge(task, size / PAGE_SIZE);
    task->proc->mmap_next = addr + size;
    mutex_unlock(&task->proc->mem_lock);
    *error = 0;
    return addr;
}

uint64_t region_map(struct task *task, size_t length, int prot, struct vnode *node, uint64_t offset, bool shared,
                    int *error) {
    int unused;
    if (!error) error = &unused;
    *error = -EINVAL;
    if (!task || length == 0 || (offset & (PAGE_SIZE - 1))) return 0;

    uint64_t size = page_align_up(length);
    if (size < length) return 0; // Overflow
    mutex_lock(&task->proc->map_mutex);
    uint64_t addr = map_locked(task, size, prot, node, offset, shared, error);
    mutex_unlock(&task->proc->map_mutex);
    return addr;
}

// Wait until no page fault of this process is working on a region's file.
// Those it was working on hold no pin of their own: a fault takes file_users
// under mem_lock while the region is there, so once the count has been seen
// at zero after the region has gone, none can still be using its file. A
// short uninterruptible sleep, not a wake-up: the caller holds map_mutex and
// must not be ended here, and a disk read is over soon.
static void wait_for_file_users(process_t *proc) {
    for (;;) {
        mutex_lock(&proc->mem_lock);
        uint32_t users = proc->file_users;
        mutex_unlock(&proc->mem_lock);
        if (users == 0) return;
        sched_sleep_uninterruptible(&proc->file_users, NULL, timer_ticks() + 1);
    }
}

// region_unmap, with the caller holding the process's map_mutex
static int unmap_locked(struct task *task, uint64_t addr, uint64_t end) {
    process_t *proc = task->proc;
    // Cutting a hole in the middle of a region leaves two: make sure there
    // is somewhere to record the second before changing anything. The
    // second holds the file, and the device, as the first does: taken now,
    // since nothing under mem_lock may touch a file.
    mem_region_t hole = { 0 };
    for (int i = 0; i < TASK_MAX_REGIONS; i++) {
        mem_region_t *r = &proc->regions[i];
        if (r->end != 0 && r->start < addr && end < r->end) {
            if (!free_slot(task)) return -ENOMEM;
            hole = *r;
        }
    }
    if (hole.end != 0) {
        pin(&hole, +1);
        hold_device(&hole, +1);
    }

    // What shared mappings in the range wrote goes to their files, once the
    // pages are unmapped (so that a page no other mapping has is left
    // clean): their files are held open meanwhile
    struct { struct vnode *node; uint64_t first, end; } written[TASK_MAX_REGIONS];
    int written_count = 0;
    for (int i = 0; i < TASK_MAX_REGIONS; i++) {
        mem_region_t *r = &proc->regions[i];
        if (r->end == 0 || !r->shared || r->end <= addr || end <= r->start) continue;
        uint64_t cut_start = r->start > addr ? r->start : addr;
        uint64_t cut_end = r->end < end ? r->end : end;
        written[written_count].node = r->node;
        file_pages(r, cut_start, cut_end, &written[written_count].first, &written[written_count].end);
        mem_region_t hold = { .node = r->node };
        pin(&hold, +1);
        written_count++;
    }

    mem_region_t gone[TASK_MAX_REGIONS];
    int gone_count = 0;
    int result;

    mutex_lock(&proc->mem_lock);
    bool busy = process_range_busy(task, addr, end);
    if (busy) {
        result = -EBUSY;
    } else {
        for (int i = 0; i < TASK_MAX_REGIONS; i++) {
            mem_region_t *r = &proc->regions[i];
            if (r->end == 0 || r->end <= addr || end <= r->start) continue;

            uint64_t cut_start = r->start > addr ? r->start : addr;
            uint64_t cut_end = r->end < end ? r->end : end;
            if (!r->device) uncharge(task, untouched_pages(task, cut_start, cut_end)); // The frames are freed below

            if (cut_start == r->start && cut_end == r->end) {
                // The entry goes before the file's pin: /proc/<pid>/maps reads
                // the entries of other processes under the tree lock, and must
                // never find one naming a file that could already be gone
                gone[gone_count++] = *r;
                memset(r, 0, sizeof(*r));
            } else if (cut_start == r->start) {
                r->file_offset += cut_end - r->start;                   // The front goes
                r->start = cut_end;
            } else if (cut_end == r->end) {
                r->end = cut_start;                                     // The back goes
            } else {
                mem_region_t *tail = free_slot(task);                   // A hole: the back becomes a region of its own
                *tail = *r;
                tail->start = cut_end;
                tail->file_offset = r->file_offset + (cut_end - r->start);
                r->end = cut_start;                                     // (Pinned above, with the device)
            }
        }

        // Whatever has frames in the range, promised or not (shared memory is
        // mapped at once and has no region)
        result = user_unmap_range(space_of(task), addr, end - addr) == 0 ? 0 : -EINVAL;
    }
    mutex_unlock(&proc->mem_lock);

    wait_for_file_users(proc);
    for (int i = 0; i < gone_count; i++) {
        hold_device(&gone[i], -1);
        pin(&gone[i], -1);
    }
    if (busy && hole.end != 0) {
        hold_device(&hole, -1);
        pin(&hole, -1);
    }
    for (int i = 0; i < written_count; i++) {
        if (!busy) pcache_writeback(written[i].node, written[i].first, written[i].end);  // (An error stays dirty: see pcache.h)
        mem_region_t hold = { .node = written[i].node };
        pin(&hold, -1);
    }
    return result;
}

int region_unmap(struct task *task, uint64_t addr, size_t length) {
    if (!task || length == 0 || (addr & (PAGE_SIZE - 1))) return -EINVAL;
    if (addr >= USER_SPACE_END || length > USER_SPACE_END - addr) return -EINVAL;
    mutex_lock(&task->proc->map_mutex);
    int result = unmap_locked(task, addr, addr + page_align_up(length));
    mutex_unlock(&task->proc->map_mutex);
    return result;
}

// region_set_brk, with the caller holding the process's map_mutex
static uint64_t set_brk_locked(struct task *task, uint64_t new_brk) {
    process_t *proc = task->proc;
    // Out-of-range requests leave the break where it is; the caller sees the
    // unchanged value and knows the request failed (Linux brk semantics).
    if (new_brk < proc->brk_start || new_brk > proc->brk_start + USER_BRK_MAX) return proc->brk_end;

    uint64_t old_top = page_align_up(proc->brk_end);
    uint64_t new_top = page_align_up(new_brk);
    if (new_top > old_top && !can_promise((new_top - old_top) / PAGE_SIZE)) return proc->brk_end;

    mutex_lock(&proc->mem_lock);
    if (new_top < old_top && process_range_busy(task, new_top, old_top)) {
        mutex_unlock(&proc->mem_lock);
        return proc->brk_end;                 // Another thread's call is using it: refused like any other
    }
    if (new_top > old_top) {
        charge(task, (new_top - old_top) / PAGE_SIZE);
    } else if (new_top < old_top) {
        uncharge(task, untouched_pages(task, new_top, old_top));
        user_unmap_range(space_of(task), new_top, old_top - new_top);
    }
    proc->brk_end = new_brk;
    mutex_unlock(&proc->mem_lock);
    return new_brk;
}

uint64_t region_set_brk(struct task *task, uint64_t new_brk) {
    mutex_lock(&task->proc->map_mutex);
    uint64_t result = set_brk_locked(task, new_brk);
    mutex_unlock(&task->proc->map_mutex);
    return result;
}

#define FAULT_AGAIN (-100)     // The table changed while the file was read: look again
#define FAULT_NO_FRAME (-101)  // No frame for an anonymous page: the page cache may have some to give

// One look at a fault on `page`. What it found to do with a file is done with
// mem_lock released and file_users counted, so the file stays pinned; then it
// looks again before mapping the page, since another thread may have given
// it a frame or unmapped the region meanwhile.
static int fault_once(struct task *task, uint64_t page) {
    process_t *proc = task->proc;
    uint64_t *space = space_of(task);
    mutex_lock(&proc->mem_lock);
    uint64_t entry = vmm_get_pte(space, page);
    if ((entry & VMM_PRESENT) || vmm_pte_swapped(entry)) {
        // Another thread got here first (or swap took it back), or it is not
        // a fault of this kind; either way the instruction can be tried again
        mutex_unlock(&proc->mem_lock);
        return 1;
    }

    int prot = 0;
    struct vnode *node = NULL;
    uint64_t file_offset = 0;
    bool found = false, shared = false, device = false;

    if (page >= proc->brk_start && page < page_align_up(proc->brk_end)) {
        prot = PROT_READ | PROT_WRITE;                    // The heap
        found = true;
    }
    for (int i = 0; i < TASK_MAX_REGIONS && !found; i++) {
        const mem_region_t *r = &proc->regions[i];
        if (r->end == 0 || page < r->start || page >= r->end) continue;
        prot = r->prot;
        node = r->node;
        shared = r->shared;
        device = r->device;
        file_offset = r->file_offset + (page - r->start);
        found = true;
    }
    if (!found) {
        mutex_unlock(&proc->mem_lock);
        return 0;
    }
    uint64_t flags = VMM_USER;
    if (prot & PROT_WRITE) flags |= VMM_WRITABLE;
    if (!(prot & PROT_EXEC)) flags |= VMM_NO_EXECUTE;

    if (!node) {
        // A zeroed frame, filled through nobody: nothing to read
        void *frame_view = vmm_alloc_user_page(space, page, flags);
        if (frame_view) uncharge(task, 1);
        mutex_unlock(&proc->mem_lock);
        return frame_view ? 1 : FAULT_NO_FRAME;
    }

    proc->file_users++;
    mutex_unlock(&proc->mem_lock);

    // From here to the next mem_lock the file may be read
    int result = 1;
    uint64_t frame = 0;
    bool charged = true;
    if (shared && file_offset >= page_align_up(node->size)) {
        // A shared mapping's page wholly past the end of its file is not the
        // file's: there is nothing to give (POSIX: SIGBUS)
        result = REGION_PAST_THE_END;
    } else if (device) {
        // The device's own page (the screen's memory), as it is: not a frame
        // of ours, so marked to be left alone by unmapping, fork and swap
        if (node->ops->map_page(node, file_offset, &frame) != 0) result = REGION_PAST_THE_END;
        flags |= VMM_SHARED | VMM_DEVICE;
        charged = false;                                   // (Nothing was promised for it)
    } else if (shared) {
        // The cached frame itself, writable if the mapping is, in every
        // process that maps the page; VMM_SHARED keeps it so across fork
        frame = pcache_get_shared(node, file_offset / PAGE_SIZE, (prot & PROT_WRITE) != 0);
        if (!frame) result = REGION_NO_MEMORY;
        if (flags & VMM_WRITABLE) flags |= VMM_SHARED;
    } else if (pcache_cacheable(node) && (file_offset & (PAGE_SIZE - 1)) == 0) {
        // A page of a file: the frame the page cache holds for it, shared
        // with every other mapping of that page. Never writable as it is: a
        // writable private mapping gets it copy-on-write, and its first
        // write makes the process a copy of its own.
        frame = pcache_get(node, file_offset / PAGE_SIZE);
        if (!frame) result = REGION_NO_MEMORY;
        if (flags & VMM_WRITABLE) flags = (flags & ~VMM_WRITABLE) | VMM_COW;
    } else {
        // A zeroed frame; then, for a file the cache does not keep, as much of
        // it as lies on this page (past its end the zeros stay). The frame is
        // filled through the kernel's own view of it, before the program can
        // see it.
        void *fresh = pmm_alloc_frame();
        if (!fresh && pcache_reclaim(16) > 0) fresh = pmm_alloc_frame();
        if (fresh) {
            frame = (uint64_t)fresh;
            void *view = (void *)(frame + pmm_get_hhdm_offset());
            memset(view, 0, PAGE_SIZE);
            vfs_read(node, file_offset, PAGE_SIZE, view);
        } else {
            result = REGION_NO_MEMORY;
        }
    }

    mutex_lock(&proc->mem_lock);
    proc->file_users--;
    if (result == 1) {
        // Is this still the page to give? Another thread may have given it
        // a frame, or unmapped the region, while the file was being read
        bool still = false;
        for (int i = 0; i < TASK_MAX_REGIONS && !still; i++) {
            const mem_region_t *r = &proc->regions[i];
            still = r->end != 0 && page >= r->start && page < r->end && r->node == node &&
                    r->file_offset + (page - r->start) == file_offset;
        }
        uint64_t now = vmm_get_pte(space, page);
        if (!still || (now & VMM_PRESENT) || vmm_pte_swapped(now)) {
            result = FAULT_AGAIN;
        } else if (!vmm_map_page(space, page, frame, flags | VMM_PRESENT)) {
            result = REGION_NO_MEMORY;
        } else {
            frame = 0;                       // The page table's now
            if (charged) uncharge(task, 1);
        }
    }
    mutex_unlock(&proc->mem_lock);
    if (frame && !device) pmm_unref_frame((void *)frame);    // Not used: the reference pcache_get or the allocator gave
    return result;
}

int region_fault(struct task *task, uint64_t addr) {
    if (!task || !task->is_user || addr >= USER_SPACE_END) return 0;
    uint64_t page = addr & ~(uint64_t)(PAGE_SIZE - 1);

    // Short of memory: cached pages nobody maps go first (they cost
    // nothing to give back), before anything else has to
    size_t free_frames = pmm_get_free_frames();
    if (free_frames < SWAP_LOW_FRAMES) pcache_reclaim(SWAP_HIGH_FRAMES - free_frames);

    for (bool reclaimed = false;;) {
        int result = fault_once(task, page);
        if (result == FAULT_AGAIN) continue;
        if (result != FAULT_NO_FRAME) return result;
        if (reclaimed || pcache_reclaim(16) == 0) return REGION_NO_MEMORY;
        reclaimed = true;
    }
}

void region_fork(struct task *child, const struct task *parent) {
    memcpy(child->proc->regions, parent->proc->regions, sizeof(child->proc->regions));
    for (int i = 0; i < TASK_MAX_REGIONS; i++) {
        if (child->proc->regions[i].end == 0) continue;
        pin(&child->proc->regions[i], +1);
        hold_device(&child->proc->regions[i], +1);     // (The parent holds it already: this cannot be refused)
    }
    // The child may touch every page the parent has not: the same promise
    // made a second time. (Not refused for lack of memory: fork has its own
    // check, and a process that cannot fork cannot run a command either.)
    child->proc->promised_pages = 0;
    charge(child, parent->proc->promised_pages);
}

// Done with a region taken off its task: what a shared mapping wrote goes
// to its file, and the file is unpinned
static void put_region(const mem_region_t *gone) {
    hold_device(gone, -1);
    if (gone->shared) {
        uint64_t first, end;
        file_pages(gone, gone->start, gone->end, &first, &end);
        pcache_writeback(gone->node, first, end);
    }
    pin(gone, -1);
}

void region_release_all(struct task *task) {
    // Each entry is cleared before its file is unpinned (see region_unmap).
    // (The last thread: nobody is left to fault, so no locks.)
    for (int i = 0; i < TASK_MAX_REGIONS; i++) {
        mem_region_t *r = &task->proc->regions[i];
        if (r->end == 0) continue;
        mem_region_t gone = *r;
        memset(r, 0, sizeof(*r));
        put_region(&gone);
    }
    uncharge(task, task->proc->promised_pages);
}

mem_region_t *region_take_all(struct task *task) {
    mem_region_t *taken = kmalloc(sizeof(task->proc->regions));
    if (!taken) {
        region_release_all(task);           // At once, then: written back while still mapped
        return NULL;
    }
    memcpy(taken, task->proc->regions, sizeof(task->proc->regions));
    memset(task->proc->regions, 0, sizeof(task->proc->regions));
    uncharge(task, task->proc->promised_pages);
    return taken;
}

void region_put_all(mem_region_t *taken) {
    if (!taken) return;
    for (int i = 0; i < TASK_MAX_REGIONS; i++) {
        if (taken[i].end != 0) put_region(&taken[i]);
    }
    kfree(taken);
}

// region_sync, with the caller holding the process's map_mutex: nobody else
// changes the table
static int sync_locked(struct task *task, uint64_t addr, size_t length) {
    uint64_t end = addr + page_align_up(length);
    // Every page of the range must be mapped by something: a region, the
    // heap, or a page that is there (the stack, shared memory)
    uint64_t *space = space_of(task);
    for (uint64_t cursor = addr; cursor < end; ) {
        uint64_t next = 0;
        if (cursor >= task->proc->brk_start && cursor < page_align_up(task->proc->brk_end)) next = page_align_up(task->proc->brk_end);
        for (int i = 0; i < TASK_MAX_REGIONS && !next; i++) {
            const mem_region_t *r = &task->proc->regions[i];
            if (r->end != 0 && cursor >= r->start && cursor < r->end) next = r->end;
        }
        if (!next) {
            uint64_t pte = vmm_get_pte(space, cursor);
            if (!(pte & VMM_PRESENT) && !vmm_pte_swapped(pte)) return -ENOMEM;
            next = cursor + PAGE_SIZE;
        }
        cursor = next;
    }

    int result = 0;
    for (int i = 0; i < TASK_MAX_REGIONS; i++) {
        const mem_region_t *r = &task->proc->regions[i];
        if (r->end == 0 || !r->shared || r->end <= addr || end <= r->start) continue;
        uint64_t first, last;
        file_pages(r, r->start > addr ? r->start : addr, r->end < end ? r->end : end, &first, &last);
        if (pcache_writeback(r->node, first, last) != 0) result = -EIO;
    }
    return result;
}

int region_sync(struct task *task, uint64_t addr, size_t length) {
    mutex_lock(&task->proc->map_mutex);
    int result = sync_locked(task, addr, length);
    mutex_unlock(&task->proc->map_mutex);
    return result;
}

void region_revalidate(struct task *task) {
    uint32_t now = pcache_truncations();
    if (!task || !task->is_user || task->proc->seen_truncations == now) return;
    process_t *proc = task->proc;
    // Under map_mutex, so that a region's file is not unpinned as its size
    // is read here. Another thread's call may be using a page that has to
    // go: then it is left until the next safe point, after that call.
    mutex_lock(&proc->map_mutex);
    mutex_lock(&proc->mem_lock);
    bool all_done = true;
    uint64_t *space = space_of(task);
    for (int i = 0; i < TASK_MAX_REGIONS; i++) {
        const mem_region_t *r = &proc->regions[i];
        if (r->end == 0 || !r->shared) continue;
        uint64_t keep = page_align_up(r->node->size);          // The file's pages: offsets below this
        if (r->file_offset + (r->end - r->start) <= keep) continue;
        uint64_t from = keep > r->file_offset ? r->start + (keep - r->file_offset) : r->start;
        size_t present = (r->end - from) / PAGE_SIZE - untouched_pages(task, from, r->end);
        if (present == 0) continue;
        if (process_range_busy(task, from, r->end)) {
            all_done = false;
            continue;
        }
        user_unmap_range(space, from, r->end - from);
        charge(task, present);           // Untouched again (and never to be given: SIGBUS)
    }
    if (all_done) proc->seen_truncations = now;
    mutex_unlock(&proc->mem_lock);
    mutex_unlock(&proc->map_mutex);
}
