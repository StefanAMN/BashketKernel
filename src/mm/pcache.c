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
 * (cow.h, ranked above it), and the contents of cached frames are copied
 * (memory reached through the direct map, which cannot fault).
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
    bool shared;                         // A shared mapping has mapped it: the file's one copy of the page
    bool dirty;                          // Mapped writable since it was last written to the file
    struct pcache_entry *hash_next;      // Same bucket (also links entries taken out, to be freed)
    struct pcache_entry *newer, *older;  // The list in order of use
} pcache_entry_t;

static pcache_entry_t *buckets[PCACHE_BUCKETS];
static pcache_entry_t *newest, *oldest;
static size_t entry_count;
static pcache_stats_t stats;
static volatile uint32_t truncations;    // Shared pages taken out by truncation (pcache_truncations)
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
    if (e->shared) e->node->shared_cached--;
    entry_count--;
    e->hash_next = *out;
    *out = e;
}

// Take out up to `want` entries mapped nowhere (only the cache owns the
// frame), oldest use first, onto `*out`. A dirty page stays: it is the
// only copy of what was written through a mapping (it is written to the
// file when the last mapping of it goes, see pcache_writeback, so one is
// left here only if that write failed). Returns how many. Lock held.
static size_t take_unmapped(size_t want, pcache_entry_t **out) {
    size_t taken = 0;
    pcache_entry_t *e = oldest;
    while (e && taken < want) {
        pcache_entry_t *next = e->newer;
        // One owner: no page table maps it, and none can start to without
        // this lock (a mapping takes its reference here, in pcache_get)
        if (pmm_get_frame_ref((void *)e->frame) == 1 && !e->dirty) {
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

// Note that a shared mapping maps `e` (and, if `writable`, may write it).
// Lock held.
static void mark_shared(pcache_entry_t *e, bool writable) {
    if (!e->shared) {
        e->shared = true;
        e->node->shared_cached++;
    }
    if (writable) e->dirty = true;
}

// pcache_get and pcache_get_shared. `shared`: 0 for a private mapping,
// which may get a frame that is not cached; 1 (read) or 2 (write) for a
// shared one, whose frame must be the cached one (every mapping of the
// page sees one frame), and which is read again until it goes in.
static uint64_t get(vnode_t *node, uint64_t index, int shared) {
    if (!node) return 0;

    for (int attempt = 0; attempt < 8; attempt++) {
        uint64_t flags = spinlock_acquire_irqsave(&pcache_lock);
        pcache_entry_t *found = find_locked(node, index);
        if (found) {
            uint64_t frame = found->frame;
            pmm_ref_frame((void *)frame);        // The caller's reference
            if (shared) mark_shared(found, shared == 2);
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
        if (entry) memset(entry, 0, sizeof(*entry));

        pcache_entry_t *taken = NULL;
        flags = spinlock_acquire_irqsave(&pcache_lock);
        found = find_locked(node, index);
        if (found || !entry || __atomic_load_n(&node->cache_generation, __ATOMIC_SEQ_CST) != generation) {
            // Another reader put the page in first (use theirs), or the file
            // changed while it was being read (what was read may be stale for
            // the next mapping, though it is as good as any for a private one)
            uint64_t theirs = 0;
            if (found) {
                theirs = found->frame;
                pmm_ref_frame((void *)theirs);
                if (shared) mark_shared(found, shared == 2);
            }
            stats.raced++;
            spinlock_release_irqrestore(&pcache_lock, flags);
            kfree(entry);
            if (theirs) {
                pmm_free_frame(frame);
                return theirs;
            }
            if (!shared) return (uint64_t)frame;          // One owner: the caller
            pmm_free_frame(frame);                        // A shared mapping needs the cached frame: again
            if (!entry) return 0;
            continue;
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
        if (shared) mark_shared(entry, shared == 2);
        stats.misses++;
        pmm_ref_frame(frame);                             // The cache's and the caller's: two owners
        size_t limit = capacity();
        if (entry_count > limit) stats.evicted += take_unmapped(entry_count - limit, &taken);
        spinlock_release_irqrestore(&pcache_lock, flags);

        free_taken(taken);
        return (uint64_t)frame;
    }
    return 0;                                             // The file kept changing under us
}

uint64_t pcache_get(vnode_t *node, uint64_t index) {
    return get(node, index, 0);
}

uint64_t pcache_get_shared(vnode_t *node, uint64_t index, bool writable) {
    if (!pcache_cacheable(node)) return 0;
    return get(node, index, writable ? 2 : 1);
}

void pcache_file_written(vnode_t *node, uint64_t offset, const void *buf, size_t len) {
    if (!pcache_cacheable(node)) return;
    // (As in pcache_drop_file: a page being read now does not go in)
    __atomic_add_fetch(&node->cache_generation, 1, __ATOMIC_SEQ_CST);
    uint64_t end = offset + len < offset ? UINT64_MAX : offset + len;

    pcache_entry_t *taken = NULL;
    uint64_t flags = spinlock_acquire_irqsave(&pcache_lock);
    for (pcache_entry_t *e = oldest; e && node->cached_pages > 0; ) {
        pcache_entry_t *next = e->newer;
        if (e->node == node) {
            if (!e->shared) {
                take_out(e, &taken);                     // As before: read again by the next mapping
                stats.dropped++;
            } else {
                // The one copy every shared mapping sees: changed in place
                uint64_t page = e->index * PAGE_SIZE;
                uint64_t lo = page > offset ? page : offset;
                uint64_t hi = page + PAGE_SIZE < end ? page + PAGE_SIZE : end;
                uint8_t *dst = (uint8_t *)(e->frame + pmm_get_hhdm_offset()) + (lo - page);
                const uint8_t *src = (const uint8_t *)buf + (lo - offset);
                if (lo < hi && dst != src) memcpy(dst, src, hi - lo);   // (Equal: pcache_writeback writing it)
            }
        }
        e = next;
    }
    spinlock_release_irqrestore(&pcache_lock, flags);
    free_taken(taken);
}

void pcache_file_gap(vnode_t *node, uint64_t old_size) {
    if (!pcache_cacheable(node) || old_size % PAGE_SIZE == 0) return;
    uint64_t flags = spinlock_acquire_irqsave(&pcache_lock);
    pcache_entry_t *e = find_locked(node, old_size / PAGE_SIZE);
    if (e && e->shared) {
        uint8_t *view = (uint8_t *)(e->frame + pmm_get_hhdm_offset());
        memset(view + old_size % PAGE_SIZE, 0, PAGE_SIZE - old_size % PAGE_SIZE);
    }
    spinlock_release_irqrestore(&pcache_lock, flags);
}

void pcache_file_truncated(vnode_t *node, uint64_t size) {
    if (!pcache_cacheable(node)) return;
    __atomic_add_fetch(&node->cache_generation, 1, __ATOMIC_SEQ_CST);

    bool shared_gone = false;
    pcache_entry_t *taken = NULL;
    uint64_t flags = spinlock_acquire_irqsave(&pcache_lock);
    for (pcache_entry_t *e = oldest; e && node->cached_pages > 0; ) {
        pcache_entry_t *next = e->newer;
        if (e->node == node) {
            uint64_t page = e->index * PAGE_SIZE;
            if (!e->shared || page >= size) {
                // Past the end, a page of a shared mapping is no longer the
                // file's: the mappings that still have it give it up at their
                // next safe point (pcache_truncations), and touching it again
                // is SIGBUS. A private page is read again, as before.
                if (e->shared) shared_gone = true;
                take_out(e, &taken);
                stats.dropped++;
            } else if (page + PAGE_SIZE > size) {
                // The page the file now ends in: past the end it reads zeros
                uint8_t *view = (uint8_t *)(e->frame + pmm_get_hhdm_offset());
                memset(view + (size - page), 0, PAGE_SIZE - (size - page));
            }
        }
        e = next;
    }
    spinlock_release_irqrestore(&pcache_lock, flags);
    free_taken(taken);
    if (shared_gone) __atomic_add_fetch(&truncations, 1, __ATOMIC_SEQ_CST);
}

uint32_t pcache_truncations(void) {
    return __atomic_load_n(&truncations, __ATOMIC_SEQ_CST);
}

void pcache_read_overlay(vnode_t *node, uint64_t offset, void *buf, size_t len) {
    if (!node || len == 0 || __atomic_load_n(&node->shared_cached, __ATOMIC_RELAXED) == 0) return;
    uint64_t end = offset + len < offset ? UINT64_MAX : offset + len;
    uint64_t flags = spinlock_acquire_irqsave(&pcache_lock);
    for (uint64_t index = offset / PAGE_SIZE; index * PAGE_SIZE < end; index++) {
        pcache_entry_t *e = find_locked(node, index);
        if (!e || !e->shared) continue;
        uint64_t page = index * PAGE_SIZE;
        uint64_t lo = page > offset ? page : offset;
        uint64_t hi = page + PAGE_SIZE < end ? page + PAGE_SIZE : end;
        const uint8_t *src = (const uint8_t *)(e->frame + pmm_get_hhdm_offset()) + (lo - page);
        memcpy((uint8_t *)buf + (lo - offset), src, hi - lo);
    }
    spinlock_release_irqrestore(&pcache_lock, flags);
}

// Write cached page `index` of `node` (frame `frame`) to the file: as much
// of it as lies before the file's end. 0, or -1 if the write failed.
static int write_page(vnode_t *node, uint64_t index, uint64_t frame) {
    uint64_t start = index * PAGE_SIZE;
    size_t size = node->size;
    if (start >= size) return 0;                     // Past the end: none of it is the file's
    size_t len = size - start < PAGE_SIZE ? size - start : PAGE_SIZE;
    const uint8_t *view = (const uint8_t *)(frame + pmm_get_hhdm_offset());
    return vfs_write(node, start, len, view) == len ? 0 : -1;
}

#define WRITEBACK_BATCH 16

int pcache_writeback(vnode_t *node, uint64_t first, uint64_t end) {
    if (!pcache_cacheable(node)) return 0;
    int result = 0;
    uint64_t cursor = first;
    for (;;) {
        // The dirty pages of the range from `cursor` on, lowest first, a
        // batch at a time; each frame gets a reference of ours so that it
        // stays while it is written with the lock released
        uint64_t index[WRITEBACK_BATCH], frame[WRITEBACK_BATCH];
        int n = 0;
        uint64_t flags = spinlock_acquire_irqsave(&pcache_lock);
        for (pcache_entry_t *e = oldest; e; e = e->newer) {
            if (e->node != node || !e->dirty || e->index < cursor || e->index >= end) continue;
            if (n == WRITEBACK_BATCH && e->index > index[n - 1]) continue;
            int at = n < WRITEBACK_BATCH ? n++ : n - 1;            // (The highest drops out)
            while (at > 0 && index[at - 1] > e->index) {
                index[at] = index[at - 1];
                frame[at] = frame[at - 1];
                at--;
            }
            index[at] = e->index;
            frame[at] = e->frame;
        }
        for (int i = 0; i < n; i++) pmm_ref_frame((void *)frame[i]);
        spinlock_release_irqrestore(&pcache_lock, flags);
        if (n == 0) break;

        bool written[WRITEBACK_BATCH];
        for (int i = 0; i < n; i++) {
            written[i] = write_page(node, index[i], frame[i]) == 0;
            if (!written[i]) result = -1;
        }
        // Clean only if no mapping has it any more (the cache and our
        // reference): one that still may write it keeps it dirty
        flags = spinlock_acquire_irqsave(&pcache_lock);
        for (int i = 0; i < n; i++) {
            pcache_entry_t *e = find_locked(node, index[i]);
            if (written[i] && e && e->frame == frame[i] && pmm_get_frame_ref((void *)frame[i]) == 2) e->dirty = false;
        }
        spinlock_release_irqrestore(&pcache_lock, flags);
        for (int i = 0; i < n; i++) pmm_unref_frame((void *)frame[i]);
        cursor = index[n - 1] + 1;
    }
    return result;
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
