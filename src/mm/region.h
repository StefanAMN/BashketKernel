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
 * Not done: shared file mappings and writing changes back to a file.
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
} mem_region_t;

/**
 * @brief Promise `length` bytes at an address of the kernel's choosing.
 * @param node   A file to take the contents from (NULL: zeros).
 * @param offset Page-aligned position in that file.
 * @param error  Receives a negative errno on failure.
 * @return The address, or 0.
 */
uint64_t region_map(struct task *task, size_t length, int prot, struct vnode *node, uint64_t offset, int *error);

/**
 * @brief Take back [addr, addr + length): the promise, and whatever frames
 *        were given. Parts of regions may be removed.
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
 *         region, -1 if it is but there is no memory left.
 */
int region_fault(struct task *task, uint64_t addr);

/**
 * @brief fork: the child gets the same regions (and the same promises).
 */
void region_fork(struct task *child, const struct task *parent);

/**
 * @brief exit and exec: forget every region. The frames themselves go with
 *        the address space.
 */
void region_release_all(struct task *task);

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
