/**
 * @file region.c
 * @brief Memory a process has been promised but not yet given (see region.h)
 *
 * A task's regions, its heap limits and its count of promised pages are
 * only ever touched by the task itself (in its system calls and its page
 * faults), by its parent before it first runs, and by whoever frees it when
 * it is dead: no lock is needed for them. The system-wide total is atomic.
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
    task->promised_pages += pages;
    __atomic_add_fetch(&promised_total, pages, __ATOMIC_RELAXED);
}

// The reverse of charge: `pages` of the task's promise have been kept (given
// a frame) or withdrawn (unmapped). Never takes more than the task has.
static void uncharge(struct task *task, size_t pages) {
    if (pages > task->promised_pages) pages = task->promised_pages;
    task->promised_pages -= pages;
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
    task->promised_pages += pages;         // Already in the system's total (region_reserve)
}

// An unused entry of the task's table of regions, or NULL if all are in use
static mem_region_t *free_slot(struct task *task) {
    for (int i = 0; i < TASK_MAX_REGIONS; i++) {
        if (task->regions[i].end == 0) return &task->regions[i];
    }
    return NULL;
}

uint64_t region_map(struct task *task, size_t length, int prot, struct vnode *node, uint64_t offset, bool shared,
                    int *error) {
    int unused;
    if (!error) error = &unused;
    *error = -EINVAL;
    if (!task || length == 0 || (offset & (PAGE_SIZE - 1))) return 0;

    uint64_t size = page_align_up(length);
    if (size < length) return 0; // Overflow
    uint64_t addr = task->mmap_next;
    *error = -ENOMEM;
    if (addr >= USER_MMAP_END || size > USER_MMAP_END - addr) return 0;

    mem_region_t *region = free_slot(task);
    if (!region) return 0;
    if (!can_promise(size / PAGE_SIZE)) return 0;

    region->start = addr;
    region->end = addr + size;
    region->prot = prot;
    region->node = node;
    region->file_offset = offset;
    region->program = false;
    region->shared = shared && node;
    pin(region, +1);
    charge(task, size / PAGE_SIZE);

    task->mmap_next = addr + size;
    *error = 0;
    return addr;
}

int region_unmap(struct task *task, uint64_t addr, size_t length) {
    if (!task || length == 0 || (addr & (PAGE_SIZE - 1))) return -EINVAL;
    if (addr >= USER_SPACE_END || length > USER_SPACE_END - addr) return -EINVAL;
    uint64_t end = addr + page_align_up(length);

    // Cutting a hole in the middle of a region leaves two: make sure there
    // is somewhere to record the second before changing anything
    for (int i = 0; i < TASK_MAX_REGIONS; i++) {
        mem_region_t *r = &task->regions[i];
        if (r->end != 0 && r->start < addr && end < r->end && !free_slot(task)) return -ENOMEM;
    }

    // What shared mappings in the range wrote goes to their files, once the
    // pages are unmapped (so that a page no other mapping has is left
    // clean): their files are held open meanwhile
    struct { struct vnode *node; uint64_t first, end; } written[TASK_MAX_REGIONS];
    int written_count = 0;
    for (int i = 0; i < TASK_MAX_REGIONS; i++) {
        mem_region_t *r = &task->regions[i];
        if (r->end == 0 || !r->shared || r->end <= addr || end <= r->start) continue;
        uint64_t cut_start = r->start > addr ? r->start : addr;
        uint64_t cut_end = r->end < end ? r->end : end;
        written[written_count].node = r->node;
        file_pages(r, cut_start, cut_end, &written[written_count].first, &written[written_count].end);
        mem_region_t hold = { .node = r->node };
        pin(&hold, +1);
        written_count++;
    }

    for (int i = 0; i < TASK_MAX_REGIONS; i++) {
        mem_region_t *r = &task->regions[i];
        if (r->end == 0 || r->end <= addr || end <= r->start) continue;

        uint64_t cut_start = r->start > addr ? r->start : addr;
        uint64_t cut_end = r->end < end ? r->end : end;
        uncharge(task, untouched_pages(task, cut_start, cut_end)); // The frames are freed below

        if (cut_start == r->start && cut_end == r->end) {
            // The entry goes before the file's pin: /proc/<pid>/maps reads
            // the entries of other processes under the tree lock, and must
            // never find one naming a file that could already be gone
            mem_region_t gone = *r;
            memset(r, 0, sizeof(*r));
            pin(&gone, -1);
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
            pin(tail, +1);
            r->end = cut_start;
        }
    }

    // Whatever has frames in the range, promised or not (shared memory is
    // mapped at once and has no region)
    int result = user_unmap_range(space_of(task), addr, end - addr) == 0 ? 0 : -EINVAL;
    for (int i = 0; i < written_count; i++) {
        pcache_writeback(written[i].node, written[i].first, written[i].end);  // (An error stays dirty: see pcache.h)
        mem_region_t hold = { .node = written[i].node };
        pin(&hold, -1);
    }
    return result;
}

uint64_t region_set_brk(struct task *task, uint64_t new_brk) {
    // Out-of-range requests leave the break where it is; the caller sees the
    // unchanged value and knows the request failed (Linux brk semantics).
    if (new_brk < task->brk_start || new_brk > task->brk_start + USER_BRK_MAX) return task->brk_end;

    uint64_t old_top = page_align_up(task->brk_end);
    uint64_t new_top = page_align_up(new_brk);
    if (new_top > old_top) {
        size_t pages = (new_top - old_top) / PAGE_SIZE;
        if (!can_promise(pages)) return task->brk_end;
        charge(task, pages);
    } else if (new_top < old_top) {
        uncharge(task, untouched_pages(task, new_top, old_top));
        user_unmap_range(space_of(task), new_top, old_top - new_top);
    }
    task->brk_end = new_brk;
    return new_brk;
}

int region_fault(struct task *task, uint64_t addr) {
    if (!task || !task->is_user || addr >= USER_SPACE_END) return 0;
    uint64_t page = addr & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t *space = space_of(task);
    uint64_t entry = vmm_get_pte(space, page);
    if ((entry & VMM_PRESENT) || vmm_pte_swapped(entry)) return 0; // Some other kind of fault

    int prot = 0;
    struct vnode *node = NULL;
    uint64_t file_offset = 0;
    bool found = false, shared = false;

    if (page >= task->brk_start && page < page_align_up(task->brk_end)) {
        prot = PROT_READ | PROT_WRITE;                    // The heap
        found = true;
    }
    for (int i = 0; i < TASK_MAX_REGIONS && !found; i++) {
        const mem_region_t *r = &task->regions[i];
        if (r->end == 0 || page < r->start || page >= r->end) continue;
        prot = r->prot;
        node = r->node;
        shared = r->shared;
        file_offset = r->file_offset + (page - r->start);
        found = true;
    }
    if (!found) return 0;
    // A shared mapping's page wholly past the end of its file is not the
    // file's: there is nothing to give (POSIX: SIGBUS)
    if (shared && file_offset >= page_align_up(node->size)) return REGION_PAST_THE_END;

    // Short of memory: cached pages nobody maps go first (they cost
    // nothing to give back), before anything else has to
    size_t free_frames = pmm_get_free_frames();
    if (free_frames < SWAP_LOW_FRAMES) pcache_reclaim(SWAP_HIGH_FRAMES - free_frames);

    uint64_t flags = VMM_USER;
    if (prot & PROT_WRITE) flags |= VMM_WRITABLE;
    if (!(prot & PROT_EXEC)) flags |= VMM_NO_EXECUTE;

    if (shared) {
        // The cached frame itself, writable if the mapping is, in every
        // process that maps the page; VMM_SHARED keeps it so across fork
        uint64_t frame = pcache_get_shared(node, file_offset / PAGE_SIZE, (prot & PROT_WRITE) != 0);
        if (!frame) return REGION_NO_MEMORY;
        if (flags & VMM_WRITABLE) flags |= VMM_SHARED;
        if (!vmm_map_page(space, page, frame, flags | VMM_PRESENT)) {
            pmm_unref_frame((void *)frame);
            return REGION_NO_MEMORY;
        }
        uncharge(task, 1);
        return 1;
    }

    if (node && pcache_cacheable(node) && (file_offset & (PAGE_SIZE - 1)) == 0) {
        // A page of a file: the frame the page cache holds for it, shared
        // with every other mapping of that page. Never writable as it is: a
        // writable private mapping gets it copy-on-write, and its first
        // write makes the process a copy of its own.
        uint64_t frame = pcache_get(node, file_offset / PAGE_SIZE);
        if (!frame) return REGION_NO_MEMORY;
        if (flags & VMM_WRITABLE) flags = (flags & ~VMM_WRITABLE) | VMM_COW;
        if (!vmm_map_page(space, page, frame, flags | VMM_PRESENT)) {
            pmm_unref_frame((void *)frame);  // The reference pcache_get gave us
            return REGION_NO_MEMORY;
        }
        uncharge(task, 1);
        return 1;
    }

    // A zeroed frame; then, for a file the cache does not keep, as much of
    // it as lies on this page (past its end the zeros stay). The frame is
    // filled through the kernel's own view of it, before the program can
    // see it.
    void *frame_view = vmm_alloc_user_page(space, page, flags);
    if (!frame_view && pcache_reclaim(16) > 0) frame_view = vmm_alloc_user_page(space, page, flags);
    if (!frame_view) return REGION_NO_MEMORY;
    if (node) vfs_read(node, file_offset, PAGE_SIZE, frame_view);

    uncharge(task, 1);
    return 1;
}

void region_fork(struct task *child, const struct task *parent) {
    memcpy(child->regions, parent->regions, sizeof(child->regions));
    for (int i = 0; i < TASK_MAX_REGIONS; i++) {
        if (child->regions[i].end != 0) pin(&child->regions[i], +1);
    }
    // The child may touch every page the parent has not: the same promise
    // made a second time. (Not refused for lack of memory: fork has its own
    // check, and a process that cannot fork cannot run a command either.)
    child->promised_pages = 0;
    charge(child, parent->promised_pages);
}

// Done with a region taken off its task: what a shared mapping wrote goes
// to its file, and the file is unpinned
static void put_region(const mem_region_t *gone) {
    if (gone->shared) {
        uint64_t first, end;
        file_pages(gone, gone->start, gone->end, &first, &end);
        pcache_writeback(gone->node, first, end);
    }
    pin(gone, -1);
}

void region_release_all(struct task *task) {
    // Each entry is cleared before its file is unpinned (see region_unmap)
    for (int i = 0; i < TASK_MAX_REGIONS; i++) {
        mem_region_t *r = &task->regions[i];
        if (r->end == 0) continue;
        mem_region_t gone = *r;
        memset(r, 0, sizeof(*r));
        put_region(&gone);
    }
    uncharge(task, task->promised_pages);
}

mem_region_t *region_take_all(struct task *task) {
    mem_region_t *taken = kmalloc(sizeof(task->regions));
    if (!taken) {
        region_release_all(task);           // At once, then: written back while still mapped
        return NULL;
    }
    memcpy(taken, task->regions, sizeof(task->regions));
    memset(task->regions, 0, sizeof(task->regions));
    uncharge(task, task->promised_pages);
    return taken;
}

void region_put_all(mem_region_t *taken) {
    if (!taken) return;
    for (int i = 0; i < TASK_MAX_REGIONS; i++) {
        if (taken[i].end != 0) put_region(&taken[i]);
    }
    kfree(taken);
}

int region_sync(struct task *task, uint64_t addr, size_t length) {
    uint64_t end = addr + page_align_up(length);
    // Every page of the range must be mapped by something: a region, the
    // heap, or a page that is there (the stack, shared memory)
    uint64_t *space = space_of(task);
    for (uint64_t cursor = addr; cursor < end; ) {
        uint64_t next = 0;
        if (cursor >= task->brk_start && cursor < page_align_up(task->brk_end)) next = page_align_up(task->brk_end);
        for (int i = 0; i < TASK_MAX_REGIONS && !next; i++) {
            const mem_region_t *r = &task->regions[i];
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
        const mem_region_t *r = &task->regions[i];
        if (r->end == 0 || !r->shared || r->end <= addr || end <= r->start) continue;
        uint64_t first, last;
        file_pages(r, r->start > addr ? r->start : addr, r->end < end ? r->end : end, &first, &last);
        if (pcache_writeback(r->node, first, last) != 0) result = -EIO;
    }
    return result;
}

void region_revalidate(struct task *task) {
    uint32_t now = pcache_truncations();
    if (!task || !task->is_user || task->seen_truncations == now) return;
    task->seen_truncations = now;
    uint64_t *space = space_of(task);
    for (int i = 0; i < TASK_MAX_REGIONS; i++) {
        const mem_region_t *r = &task->regions[i];
        if (r->end == 0 || !r->shared) continue;
        uint64_t keep = page_align_up(r->node->size);          // The file's pages: offsets below this
        if (r->file_offset + (r->end - r->start) <= keep) continue;
        uint64_t from = keep > r->file_offset ? r->start + (keep - r->file_offset) : r->start;
        size_t present = (r->end - from) / PAGE_SIZE - untouched_pages(task, from, r->end);
        if (present == 0) continue;
        user_unmap_range(space, from, r->end - from);
        charge(task, present);           // Untouched again (and never to be given: SIGBUS)
    }
}
