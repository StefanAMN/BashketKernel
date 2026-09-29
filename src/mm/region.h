/**
 * @file region.h
 * @brief Memory a process has been promised but not yet given
 *
 * mmap and brk do not hand out memory any more: they record a region of the
 * address space ("these addresses are yours, readable and writable") and
 * return. A page of it gets a frame the first time the program touches it,
 * through the page fault that touch causes. A program that maps a gigabyte
 * and uses three pages costs three frames.
 *
 * A region may also stand for part of a file: the page then starts out with
 * the file's contents, read when the page is first touched. execve uses
 * this for a program's code and constants (`program` regions), so a page
 * of code is read when it first runs. Such a mapping
 * is private: writing to it changes the process's copy, never the file, and
 * later changes to the file do not show in pages already read.
 *
 * Promises have a limit. Each untouched page is counted, and mmap or brk
 * fails when the pages promised to all processes would exceed the frames
 * that are free. That makes running out of memory something a program is
 * told about (a failed call) rather than something that happens to it (a
 * kill at the first touch). It is not a guarantee: fork, the kernel itself
 * and copy-on-write also use frames, and none of those is counted here.
 *
 * The pages of a file come from the page cache (pcache.h): every process
 * mapping one maps the same frame, read-only, or copy-on-write in a
 * writable mapping, so the first write gives the process its own copy.
 *
 * A shared mapping of a file (MAP_SHARED) maps the cached frame itself,
 * writable if the mapping is, in every process that maps the page: they
 * see each other's writes at once, and read() and write() on the file see
 * them too (pcache.h). What was written reaches the file at msync
 * (region_sync), when the range is unmapped, and when the process exits or
 * runs another program (so also at shutdown, which ends every process).
 * Such a page is never swapped (its frame is shared with the cache). A
 * page wholly past the file's end is SIGBUS: at the touch if it was never
 * mapped, and once a truncation has cut it off, from the process's next
 * safe point on (region_revalidate).
 */

#ifndef REGION_H
#define REGION_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

struct task;
struct vnode;

#define TASK_MAX_REGIONS 36   // 32 for mmap, and the segments execve leaves to be read later (ELF_LAZY_SEGMENTS)

typedef struct {
    uint64_t start, end;        // Page-aligned; end == 0 marks an unused entry
    int prot;                   // PROT_READ | PROT_WRITE | PROT_EXEC
    struct vnode *node;         // The file behind it, or NULL for zero-filled memory
    uint64_t file_offset;       // Where in the file `start` is
    bool program;               // Part of the program itself (execve), not of mmap: while it
                                // exists the file may not be written (vnode exec_count)
    bool shared;                // MAP_SHARED: writes reach the file; while writable, the file
                                // counts as open for writing (vnode write_count: not runnable)
} mem_region_t;

#define REGION_NO_MEMORY    (-1)  // region_fault: promised, but there is no frame for it
#define REGION_PAST_THE_END (-2)  // region_fault: a shared mapping's page past its file's end (SIGBUS)

/**
 * @brief Promise `length` bytes at an address of the kernel's choosing.
 * @param node   A file to take the contents from (NULL: zeros).
 * @param offset Page-aligned position in that file.
 * @param shared A shared mapping of `node` (which must be cacheable).
 * @param error  Receives a negative errno on failure.
 * @return The address, or 0.
 */
uint64_t region_map(struct task *task, size_t length, int prot, struct vnode *node, uint64_t offset, bool shared,
                    int *error);

/**
 * @brief Take back [addr, addr + length): the promise, and whatever frames
 *        were given. Parts of regions may be removed. What a shared mapping
 *        wrote in the range is written to its file. May sleep.
 * @return 0, or a negative errno.
 */
int region_unmap(struct task *task, uint64_t addr, size_t length);

/**
 * @brief Move the end of the heap. Growing only promises; shrinking gives
 *        frames back. Returns the new break, or the old one if refused.
 */
uint64_t region_set_brk(struct task *task, uint64_t new_brk);

/**
 * @brief Keep the promise for the page containing `addr`: give it a frame
 *        (and its file contents). May sleep (reading a file from disk).
 * @return 1 if the page is now present, 0 if `addr` is not in any promised
 *         region, REGION_NO_MEMORY if it is but there is no memory left,
 *         REGION_PAST_THE_END for a shared mapping's page past the end of
 *         its file.
 */
int region_fault(struct task *task, uint64_t addr);

/**
 * @brief msync: write to their files what the task's shared mappings in
 *        [addr, addr + length) have written. May sleep.
 * @return 0; -ENOMEM if part of the range is mapped by nothing; -EIO if a
 *         page could not be written.
 */
int region_sync(struct task *task, uint64_t addr, size_t length);

/**
 * @brief If a truncation has happened since the task last looked
 *        (pcache_truncations), give up the pages of its shared mappings
 *        that now lie past the end of their files: touching one again is
 *        SIGBUS. Called by the task itself at a safe point.
 */
void region_revalidate(struct task *task);

/**
 * @brief fork: the child gets the same regions (and the same promises).
 */
void region_fork(struct task *child, const struct task *parent);

/**
 * @brief exit and exec: forget every region, writing what shared mappings
 *        wrote to their files. Call once the address space is gone (or no
 *        longer the task's), so that pages no mapping holds any more are
 *        left clean. May sleep.
 */
void region_release_all(struct task *task);

/**
 * @brief exec: take every region off the task (its table is empty and its
 *        promise given back), the files staying held, so that what shared
 *        mappings wrote can be written once the old address space is gone
 *        (region_put_all). NULL if memory ran out: then they were released
 *        at once, as region_release_all.
 */
mem_region_t *region_take_all(struct task *task);

/**
 * @brief Finish what region_take_all began: write back, unpin, free.
 *        NULL does nothing. May sleep.
 */
void region_put_all(mem_region_t *taken);

/**
 * @brief Pages promised to all processes and not yet touched.
 */
size_t region_promised_pages(void);

/**
 * @brief Promise `pages` pages before there is a task to promise them to
 *        (execve builds the new program's address space first). Refused,
 *        as mmap would be, if that many cannot be promised. Give them to a
 *        task with region_adopt, or back with region_unreserve.
 */
bool region_reserve(size_t pages);

/**
 * @brief Take back pages region_reserve promised and nobody adopted.
 */
void region_unreserve(size_t pages);

/**
 * @brief Count `node` as the file of `by` (+1 or -1) more program regions:
 *        open (it cannot be removed) and running (it cannot be opened for
 *        writing or truncated: ETXTBSY). Takes the tree lock (which nests).
 */
void region_hold_program(struct vnode *node, int by);

/**
 * @brief Give `task` the program regions execve built: `count` entries,
 *        whose files are already held (region_hold_program) and whose
 *        `pages` untouched pages were reserved (region_reserve); both now
 *        belong to the task. The task must have room for them: a new task,
 *        or one whose regions were just released.
 */
void region_adopt(struct task *task, const mem_region_t *regions, uint32_t count, size_t pages);

#endif // REGION_H
