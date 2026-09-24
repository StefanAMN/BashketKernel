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
 * by every drop, so that a page read while the file was being changed is
 * not put in the cache afterwards with the old contents.
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
