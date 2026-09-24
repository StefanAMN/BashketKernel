/**
 * @file pcache.c
 * @brief The page cache (see pcache.h)
 *
 * Entries live in a hash table keyed by (file, page index) and on one list
 * in order of use, most recent first. Both, and every file's count of
 * cached pages, are protected by pcache_lock. Nothing is read from a file,
 * allocated or freed (frame or entry) while it is held: a page is read
 * first and put in afterwards, and what is taken out is freed after the
 * lock is released. Under it only the frame sharing counts are touched
 * (cow.h, ranked above it).
 */

#include "pcache.h"
#include "vfs.h"
#include "pmm.h"
#include "cow.h"
#include "heap.h"
#include "kstring.h"
#include "spinlock.h"

#define PCACHE_BUCKETS 1024

typedef struct pcache_entry {
    vnode_t *node;
    uint64_t index;                      // Page of the file: bytes index*PAGE_SIZE onwards
    uint64_t frame;                      // Physical address; the cache holds one reference to it
    struct pcache_entry *hash_next;      // Same bucket (also links entries taken out, to be freed)
    struct pcache_entry *newer, *older;  // The list in order of use
} pcache_entry_t;

static pcache_entry_t *buckets[PCACHE_BUCKETS];
static pcache_entry_t *newest, *oldest;
static size_t entry_count;
static pcache_stats_t stats;
static spinlock_t pcache_lock = SPINLOCK_RANKED("pcache", LOCK_RANK_PCACHE);

bool pcache_cacheable(const vnode_t *node) {
    // A node with operations but no truncate is a device or a /proc file
    // (as vfs_truncate decides): what it reads is not data that stays put
    return node && node->type == VFS_FILE && (!node->ops || node->ops->truncate);
}

// The bucket for page `index` of `node`
static pcache_entry_t **bucket_of(const vnode_t *node, uint64_t index) {
    uint64_t h = ((uint64_t)node >> 4) ^ (index * 0x9E3779B97F4A7C15ULL);
    return &buckets[(h ^ (h >> 32)) % PCACHE_BUCKETS];
}

// The entry for page `index` of `node`, or NULL. Lock held.
static pcache_entry_t *find_locked(const vnode_t *node, uint64_t index) {
    for (pcache_entry_t *e = *bucket_of(node, index); e; e = e->hash_next) {
        if (e->node == node && e->index == index) return e;
    }
    return NULL;
}

// Take `e` off the list of use. Lock held.
static void unlink_use(pcache_entry_t *e) {
    if (e->newer) e->newer->older = e->older; else newest = e->older;
    if (e->older) e->older->newer = e->newer; else oldest = e->newer;
    e->newer = e->older = NULL;
}

// Put `e` at the front of the list of use (just used). Lock held.
static void mark_used(pcache_entry_t *e) {
    e->older = newest;
    e->newer = NULL;
    if (newest) newest->newer = e; else oldest = e;
    newest = e;
}

// Take `e` out of the cache entirely and push it onto `*out` (linked by
// hash_next) for the caller to free once the lock is released. Lock held.
static void take_out(pcache_entry_t *e, pcache_entry_t **out) {
    pcache_entry_t **link = bucket_of(e->node, e->index);
    while (*link != e) link = &(*link)->hash_next;
    *link = e->hash_next;
    unlink_use(e);
    e->node->cached_pages--;
    entry_count--;
    e->hash_next = *out;
    *out = e;
}

// Take out up to `want` entries mapped nowhere (only the cache owns the
// frame), oldest use first, onto `*out`. Returns how many. Lock held.
static size_t take_unmapped(size_t want, pcache_entry_t **out) {
    size_t taken = 0;
    pcache_entry_t *e = oldest;
    while (e && taken < want) {
        pcache_entry_t *next = e->newer;
        // One owner: no page table maps it, and none can start to without
        // this lock (a mapping takes its reference here, in pcache_get)
        if (pmm_get_frame_ref((void *)e->frame) == 1) {
            take_out(e, out);
            taken++;
        }
        e = next;
    }
    return taken;
}

// Give back the frames and the memory of entries taken out. No lock held.
static void free_taken(pcache_entry_t *list) {
    while (list) {
        pcache_entry_t *next = list->hash_next;
        pmm_unref_frame((void *)list->frame);  // The cache's reference: the frame is freed if that was the last
        kfree(list);
        list = next;
    }
}

// How many entries the cache keeps before it starts giving back those
// mapped nowhere (see PCACHE_SHARE)
static size_t capacity(void) {
    size_t share = pmm_get_total_frames() / PCACHE_SHARE;
    return share > PCACHE_MIN ? share : PCACHE_MIN;
}

uint64_t pcache_get(vnode_t *node, uint64_t index) {
    if (!node) return 0;

    uint64_t flags = spinlock_acquire_irqsave(&pcache_lock);
    pcache_entry_t *found = find_locked(node, index);
    if (found) {
        uint64_t frame = found->frame;
        pmm_ref_frame((void *)frame);        // The caller's reference
        unlink_use(found);
        mark_used(found);
        stats.hits++;
        spinlock_release_irqrestore(&pcache_lock, flags);
        return frame;
    }
    // Noted before the file is read: if it changes while we read, this
    // number will have moved on by the time the page would go in
    uint32_t generation = __atomic_load_n(&node->cache_generation, __ATOMIC_SEQ_CST);
    spinlock_release_irqrestore(&pcache_lock, flags);

    void *frame = pmm_alloc_frame();
    if (!frame && pcache_reclaim(1) > 0) frame = pmm_alloc_frame();
    if (!frame) return 0;
    uint8_t *view = (uint8_t *)((uint64_t)frame + pmm_get_hhdm_offset());
    memset(view, 0, PAGE_SIZE);                       // Past the end of the file: zeros
    vfs_read(node, index * PAGE_SIZE, PAGE_SIZE, view);
    pcache_entry_t *entry = kmalloc(sizeof(*entry));  // (None: the page is the caller's alone, uncached)

    pcache_entry_t *taken = NULL;
    flags = spinlock_acquire_irqsave(&pcache_lock);
    found = find_locked(node, index);
    if (found || !entry || __atomic_load_n(&node->cache_generation, __ATOMIC_SEQ_CST) != generation) {
        // Another reader put the page in first (use theirs), or the file
        // changed while it was being read (what was read may be stale for
        // the next mapping, though it is as good as any for this one)
        uint64_t theirs = 0;
        if (found) {
            theirs = found->frame;
            pmm_ref_frame((void *)theirs);
        }
        stats.raced++;
        spinlock_release_irqrestore(&pcache_lock, flags);
        kfree(entry);
        if (theirs) {
            pmm_free_frame(frame);
            return theirs;
        }
        return (uint64_t)frame;                       // One owner: the caller
    }
    entry->node = node;
    entry->index = index;
    entry->frame = (uint64_t)frame;
    pcache_entry_t **bucket = bucket_of(node, index);
    entry->hash_next = *bucket;
    *bucket = entry;
    mark_used(entry);
    node->cached_pages++;
    entry_count++;
    stats.misses++;
    pmm_ref_frame(frame);                             // The cache's and the caller's: two owners
    size_t limit = capacity();
    if (entry_count > limit) stats.evicted += take_unmapped(entry_count - limit, &taken);
    spinlock_release_irqrestore(&pcache_lock, flags);

    free_taken(taken);
    return (uint64_t)frame;
}

void pcache_drop_file(vnode_t *node) {
    if (!pcache_cacheable(node)) return;
    // Raised before the lock is taken and before anything is looked at:
    // a page being read now goes in only if its reader takes the lock
    // first, and then the walk below finds it
    __atomic_add_fetch(&node->cache_generation, 1, __ATOMIC_SEQ_CST);

    pcache_entry_t *taken = NULL;
    uint64_t flags = spinlock_acquire_irqsave(&pcache_lock);
    if (node->cached_pages > 0) {
        pcache_entry_t *e = oldest;
        while (e && node->cached_pages > 0) {
            pcache_entry_t *next = e->newer;
            if (e->node == node) {
                take_out(e, &taken);
                stats.dropped++;
            }
            e = next;
        }
    }
    spinlock_release_irqrestore(&pcache_lock, flags);
    // Frames still mapped somewhere stay with their mappings, which let go
    // of them as they always have; the rest are freed here
    free_taken(taken);
}

size_t pcache_reclaim(size_t want) {
    if (want == 0) return 0;
    pcache_entry_t *taken = NULL;
    uint64_t flags = spinlock_acquire_irqsave(&pcache_lock);
    size_t n = take_unmapped(want, &taken);
    stats.evicted += n;
    spinlock_release_irqrestore(&pcache_lock, flags);
    free_taken(taken);
    return n;
}

size_t pcache_pages(void) {
    uint64_t flags = spinlock_acquire_irqsave(&pcache_lock);
    size_t n = entry_count;
    spinlock_release_irqrestore(&pcache_lock, flags);
    return n;
}

size_t pcache_unmapped_pages(void) {
    size_t n = 0;
    uint64_t flags = spinlock_acquire_irqsave(&pcache_lock);
    for (pcache_entry_t *e = oldest; e; e = e->newer) {
        if (pmm_get_frame_ref((void *)e->frame) == 1) n++;
    }
    spinlock_release_irqrestore(&pcache_lock, flags);
    return n;
}

const pcache_stats_t *pcache_stats(void) {
    return &stats;
}
