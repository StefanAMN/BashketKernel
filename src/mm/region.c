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

static size_t promised_total = 0;

size_t region_promised_pages(void) {
    return __atomic_load_n(&promised_total, __ATOMIC_RELAXED);
}

static uint64_t page_up(uint64_t value) {
    return (value + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
}

static uint64_t *space_of(const struct task *task) {
    return vmm_space_from_phys((uint64_t)task->pml4_phys);
}

static void charge(struct task *task, size_t pages) {
    task->promised_pages += pages;
    __atomic_add_fetch(&promised_total, pages, __ATOMIC_RELAXED);
}

static void uncharge(struct task *task, size_t pages) {
    if (pages > task->promised_pages) pages = task->promised_pages;
    task->promised_pages -= pages;
    __atomic_sub_fetch(&promised_total, pages, __ATOMIC_RELAXED);
}

// May this many more pages be promised? Only what is free now, less what
// has been promised already and the reserve fork insists on, can be.
static bool can_promise(size_t pages) {
    size_t free_frames = pmm_get_free_frames();
    size_t spoken_for = region_promised_pages() + PROCESS_FORK_RESERVE_FRAMES;
    return free_frames > spoken_for && pages <= free_frames - spoken_for;
}

// Pages of [start, end) that have no frame yet
static size_t untouched_pages(const struct task *task, uint64_t start, uint64_t end) {
    size_t count = 0;
    uint64_t *space = space_of(task);
    for (uint64_t page = start; page < end; page += PAGE_SIZE) {
        if (!(vmm_get_pte(space, page) & VMM_PRESENT)) count++;
    }
    return count;
}

// A region's file must not be removed while the region exists: it counts as
// open, like a file descriptor or a working directory
static void pin(struct vnode *node, int by) {
    if (!node) return;
    vfs_lock();
    node->open_count += by;
    vfs_unlock();
}

static mem_region_t *free_slot(struct task *task) {
    for (int i = 0; i < TASK_MAX_REGIONS; i++) {
        if (task->regions[i].end == 0) return &task->regions[i];
    }
    return NULL;
}

uint64_t region_map(struct task *task, size_t length, int prot, struct vnode *node, uint64_t offset, int *error) {
    int unused;
    if (!error) error = &unused;
    *error = -EINVAL;
    if (!task || length == 0 || (offset & (PAGE_SIZE - 1))) return 0;

    uint64_t size = page_up(length);
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
    pin(node, +1);
    charge(task, size / PAGE_SIZE);

    task->mmap_next = addr + size;
    *error = 0;
    return addr;
}

int region_unmap(struct task *task, uint64_t addr, size_t length) {
    if (!task || length == 0 || (addr & (PAGE_SIZE - 1))) return -EINVAL;
    if (addr >= USER_SPACE_END || length > USER_SPACE_END - addr) return -EINVAL;
    uint64_t end = addr + page_up(length);

    // Cutting a hole in the middle of a region leaves two: make sure there
    // is somewhere to record the second before changing anything
    for (int i = 0; i < TASK_MAX_REGIONS; i++) {
        mem_region_t *r = &task->regions[i];
        if (r->end != 0 && r->start < addr && end < r->end && !free_slot(task)) return -ENOMEM;
    }

    for (int i = 0; i < TASK_MAX_REGIONS; i++) {
        mem_region_t *r = &task->regions[i];
        if (r->end == 0 || r->end <= addr || end <= r->start) continue;

        uint64_t cut_start = r->start > addr ? r->start : addr;
        uint64_t cut_end = r->end < end ? r->end : end;
        uncharge(task, untouched_pages(task, cut_start, cut_end)); // The frames are freed below

        if (cut_start == r->start && cut_end == r->end) {
            pin(r->node, -1);
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
            pin(tail->node, +1);
            r->end = cut_start;
        }
    }

    // Whatever has frames in the range, promised or not (shared memory is
    // mapped at once and has no region)
    return user_unmap_range(space_of(task), addr, end - addr) == 0 ? 0 : -EINVAL;
}

uint64_t region_set_brk(struct task *task, uint64_t new_brk) {
    // Out-of-range requests leave the break where it is; the caller sees the
    // unchanged value and knows the request failed (Linux brk semantics).
    if (new_brk < task->brk_start || new_brk > task->brk_start + USER_BRK_MAX) return task->brk_end;

    uint64_t old_top = page_up(task->brk_end);
    uint64_t new_top = page_up(new_brk);
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
    if (vmm_get_pte(space, page) & VMM_PRESENT) return 0; // Some other kind of fault

    int prot = 0;
    struct vnode *node = NULL;
    uint64_t file_offset = 0;
    bool found = false;

    if (page >= task->brk_start && page < page_up(task->brk_end)) {
        prot = PROT_READ | PROT_WRITE;                    // The heap
        found = true;
    }
    for (int i = 0; i < TASK_MAX_REGIONS && !found; i++) {
        const mem_region_t *r = &task->regions[i];
        if (r->end == 0 || page < r->start || page >= r->end) continue;
        prot = r->prot;
        node = r->node;
        file_offset = r->file_offset + (page - r->start);
        found = true;
    }
    if (!found) return 0;

    uint64_t flags = VMM_USER;
    if (prot & PROT_WRITE) flags |= VMM_WRITABLE;
    if (!(prot & PROT_EXEC)) flags |= VMM_NO_EXECUTE;

    // A zeroed frame; then, for a file, as much of the file as lies on this
    // page (past its end the zeros stay). The frame is filled through the
    // kernel's own view of it, before the program can see it.
    void *frame_view = vmm_alloc_user_page(space, page, flags);
    if (!frame_view) return -1;
    if (node) vfs_read(node, file_offset, PAGE_SIZE, frame_view);

    uncharge(task, 1);
    return 1;
}

void region_fork(struct task *child, const struct task *parent) {
    memcpy(child->regions, parent->regions, sizeof(child->regions));
    for (int i = 0; i < TASK_MAX_REGIONS; i++) {
        if (child->regions[i].end != 0) pin(child->regions[i].node, +1);
    }
    // The child may touch every page the parent has not: the same promise
    // made a second time. (Not refused for lack of memory: fork has its own
    // check, and a process that cannot fork cannot run a command either.)
    child->promised_pages = 0;
    charge(child, parent->promised_pages);
}

void region_release_all(struct task *task) {
    for (int i = 0; i < TASK_MAX_REGIONS; i++) {
        if (task->regions[i].end != 0) pin(task->regions[i].node, -1);
    }
    memset(task->regions, 0, sizeof(task->regions));
    uncharge(task, task->promised_pages);
}
