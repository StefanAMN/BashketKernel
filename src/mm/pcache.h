/**
 * @file pcache.h
 * @brief The page cache: file pages read once and shared by every mapping
 *
 * A page of a file that a process maps (its program's code, or a file it
 * mapped privately with mmap) is read into a frame the first time anyone
 * touches it, and that frame is kept here, keyed by the file and the page's
 * index in it. Every later mapping of the same page maps the same frame:
 * two shells running at once hold one copy of the shell's code, and a
 * program run a second time is not read from the disk again.
 *
 * Who owns a cached frame is counted with the frame sharing counts (cow.h):
 * the cache holds one reference, and each page-table entry that maps it one
 * more. A frame with one owner is therefore "mapped nowhere": only the
 * cache has it, and it is the only kind the cache ever frees. Frames mapped
 * by a process are freed, as before, when the last mapping goes, so a frame
 * is never reused while a page table still points at it.
 *
 * Coherence: when a file is written or truncated, its cached pages are
 * dropped (the cache lets go of its reference), and the next mapping reads
 * the file again. Pages already mapped keep what they held, as private
 * mappings always have here (region.h); a running program's file cannot be
 * written at all (ETXTBSY). Each file carries a generation number, raised
 * by every change, so that a page read while the file was being changed is
 * not put in the cache afterwards with the old contents.
 *
 * Shared mappings (MAP_SHARED) are the exception: a page one of them has
 * mapped is the file's one copy of that page, mapped writable by every
 * shared mapping of it, so it cannot be dropped. write() changes it in
 * place (pcache_file_written), read() sees what was written through the
 * mappings and not yet saved (pcache_read_overlay), and truncation takes
 * it out only past the new end (pcache_file_truncated). A page mapped
 * writable is dirty until pcache_writeback has written it to the file
 * with no mapping left that could write it again: an msync, munmap or exit
 * writes it (region.c), and eviction leaves it alone. Which mapping wrote
 * is not known (that would mean every page table that maps the frame, and
 * no reverse map is kept): a dirty page still mapped is written again by
 * the next msync, whether or not it changed.
 *
 * Writable private mappings map a cached frame read-only and copy-on-write
 * (VMM_COW): the first write gives the process its own copy, as after fork.
 *
 * Eviction: pages mapped nowhere are freed, oldest use first, when the cache
 * holds more than a share of memory (PCACHE_SHARE), and when memory runs
 * short (pcache_reclaim, called where frames are about to be needed: a
 * user page fault with memory low, mmap and brk before they refuse, fork
 * before it refuses). Swap never takes a cached page (it is shared with
 * the cache, and swap leaves shared frames alone).
 *
 * Only files whose contents are data are cached: files in memory, and files
 * of a disk filesystem (those whose node has a truncate operation). Devices
 * and /proc files, which say something new at each read, are not.
 */

#ifndef PCACHE_H
#define PCACHE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

struct vnode;

#define PCACHE_SHARE 32   // The cache holds at most 1/32 of memory in pages mapped nowhere (at least PCACHE_MIN)
#define PCACHE_MIN   128

typedef struct {
    uint64_t hits;           // Mappings given a frame that was already cached
    uint64_t misses;         // Pages read from their file into the cache
    uint64_t evicted;        // Pages mapped nowhere given back (at the cap or under pressure)
    uint64_t dropped;        // Pages let go because their file changed or went away
    uint64_t raced;          // Pages read but not cached: the file changed meanwhile, or another reader was first
} pcache_stats_t;

/**
 * @brief Whether pages of `node` may be cached: a regular file whose
 *        contents are data (in memory, or on a disk filesystem).
 */
bool pcache_cacheable(const struct vnode *node);

/**
 * @brief The frame holding page `index` (bytes index*4096 onwards, zeros
 *        past the end of the file) of `node`, read from the file if it is
 *        not cached. The caller gets a reference of its own (one more owner
 *        of the frame): it maps the frame, or gives the reference back with
 *        pmm_unref_frame. The file must be held open (a region pins it).
 *        May sleep (reads the file); call with no spinlock held.
 * @return The frame's physical address, or 0 if there was no memory.
 */
uint64_t pcache_get(struct vnode *node, uint64_t index);

/**
 * @brief The contents of `node` have changed, or it is about to be freed:
 *        let go of its cached pages. Cheap when it has none. Call after the
 *        change (a page read during it is then not cached), and before the
 *        node is freed. No spinlock may be held; the tree lock may be.
 */
void pcache_drop_file(struct vnode *node);

/**
 * @brief pcache_get for a shared mapping: the cached frame itself (read
 *        again if the file changes meanwhile), marked as shared and, if
 *        `writable`, dirty. Same reference and sleeping rules.
 * @return The frame, or 0 if there was no memory (or the file would not
 *         stop changing, or cannot be cached).
 */
uint64_t pcache_get_shared(struct vnode *node, uint64_t index, bool writable);

/**
 * @brief `len` bytes of `buf` have been written to `node` at `offset`:
 *        pages shared mappings have mapped take them in place, other
 *        cached pages of the file are dropped. `buf` is kernel memory.
 *        Call after the write. No spinlock may be held; the tree lock may be.
 */
void pcache_file_written(struct vnode *node, uint64_t offset, const void *buf, size_t len);

/**
 * @brief `node` now ends at `size`: its pages past that go (a shared
 *        mapping's included: see pcache_truncations), the page it ends in
 *        reads zeros past the end, and other cached pages are dropped.
 *        Call after the change. Same lock rules as pcache_file_written.
 */
void pcache_file_truncated(struct vnode *node, uint64_t size);

/**
 * @brief How many truncations have taken pages of shared mappings out of
 *        the cache. A task that has seen a different number gives up its
 *        own pages past the end of its files (region_revalidate).
 */
uint32_t pcache_truncations(void);

/**
 * @brief Copy into buf[0..len) (just read from `node` at `offset`) the
 *        bytes of pages shared mappings have mapped, which may hold what
 *        was written through them and not yet written to the file. Cheap
 *        when the file has none. No spinlock may be held.
 */
void pcache_read_overlay(struct vnode *node, uint64_t offset, void *buf, size_t len);

/**
 * @brief Write the dirty pages of `node` with indices in [first, end) to
 *        the file (each up to the file's end); one no mapping holds any
 *        more is clean afterwards. The file must be held open. Sleeps
 *        (writes the file); no spinlock may be held.
 * @return 0, or -1 if a page could not be written (it stays dirty).
 */
int pcache_writeback(struct vnode *node, uint64_t first, uint64_t end);

/**
 * @brief Free up to `want` cached pages that are mapped nowhere, least
 *        recently used first. Returns how many were freed. No spinlock may
 *        be held.
 */
size_t pcache_reclaim(size_t want);

/**
 * @brief Pages in the cache, mapped or not.
 */
size_t pcache_pages(void);

/**
 * @brief Pages in the cache mapped nowhere: free memory in all but name
 *        (pcache_reclaim can have them back at once). Walks the cache.
 */
size_t pcache_unmapped_pages(void);

/**
 * @brief Counts since boot (see pcache_stats_t).
 */
const pcache_stats_t *pcache_stats(void);

#endif // PCACHE_H
