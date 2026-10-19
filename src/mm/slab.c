/**
 * @file slab.c
 * @brief Implementation of High-Performance Kernel Object Slab Allocator
 */

#include "slab.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "kprintf.h"
#include "kstring.h"
#include "spinlock.h"

// A slab is one 4 KiB page: this header at its start, then as many objects
// as fit. Free objects are chained through their own first 8 bytes.
typedef struct slab_page {
    struct kmem_cache *cache;   // The cache this page belongs to
    void *freelist;             // Linked list of free objects in this page
    size_t inuse;               // Count of currently allocated objects
    struct slab_page *next;     // Next page in the same list (full or partial)
} slab_page_t;

struct kmem_cache {
    char name[32];              // For diagnostics
    size_t object_size;         // Size the creator asked for
    size_t aligned_size;        // Distance between objects (size rounded up to the alignment)
    size_t align;               // Alignment of every object
    size_t first_offset;        // Offset of the first object in a page
    size_t objects_per_slab;    // How many objects one page holds
    size_t pages;               // Pages currently held
    size_t objects_in_use;      // Objects currently allocated
    slab_page_t *slabs_full;    // Pages with no free object
    slab_page_t *slabs_partial; // Pages with at least one free object
    struct kmem_cache *next;    // All caches, for diagnostics
};

static kmem_cache_t *cache_chain = NULL;

// One lock for all caches. Lock order: slab_lock, then the PMM lock.
static spinlock_t slab_lock = SPINLOCK_RANKED("slab", LOCK_RANK_HEAP);

// The slab an object belongs to. Every slab is one page with its header at
// the start, so it is the object's address rounded down to a page.
static slab_page_t *slab_of(const void *obj) {
    return (slab_page_t *)((uint64_t)obj & ~(uint64_t)(PAGE_SIZE - 1));
}

// Take a frame from the frame allocator and make it a slab for `cache`: the
// header at the start, then as many objects as fit, all on the free list.
// NULL if no frame is free. Caller holds slab_lock.
static slab_page_t *slab_page_create(kmem_cache_t *cache) {
    void *phys_frame = pmm_alloc_frame();
    if (!phys_frame) return NULL;

    slab_page_t *slab = (slab_page_t *)((uint64_t)phys_frame + pmm_get_hhdm_offset());
    slab->cache = cache;
    slab->inuse = 0;
    slab->next = NULL;
    slab->freelist = NULL;

    // Chain every object slot into the free list, lowest address first
    uint8_t *page = (uint8_t *)slab;
    for (size_t i = cache->objects_per_slab; i > 0; i--) {
        void *obj = page + cache->first_offset + (i - 1) * cache->aligned_size;
        *(void **)obj = slab->freelist;
        slab->freelist = obj;
    }

    cache->pages++;
    return slab;
}

// Take `slab` out of the singly linked list `*list` (nothing happens if it
// is not there). Caller holds slab_lock.
static void unlink_slab(slab_page_t **list, slab_page_t *slab) {
    while (*list && *list != slab) list = &(*list)->next;
    if (*list) *list = slab->next;
}

kmem_cache_t *kmem_cache_create(const char *name, size_t object_size, size_t align) {
    if (object_size == 0) return NULL;
    if (align < 8) align = 8;
    if (align & (align - 1)) return NULL;      // Must be a power of two
    if (align > 1024) return NULL;

    size_t aligned_size = (object_size + align - 1) & ~(align - 1);
    size_t first_offset = (sizeof(slab_page_t) + align - 1) & ~(align - 1);
    if (aligned_size < object_size) return NULL;                   // Overflowed
    if (first_offset + aligned_size > PAGE_SIZE) return NULL;      // Not even one object fits in a page

    kmem_cache_t *cache = (kmem_cache_t *)kmalloc(sizeof(kmem_cache_t));
    if (!cache) return NULL;
    memset(cache, 0, sizeof(kmem_cache_t));

    if (name) {
        size_t i = 0;
        for (; name[i] != '\0' && i < sizeof(cache->name) - 1; i++) {
            cache->name[i] = name[i];
        }
        cache->name[i] = '\0';
    }

    cache->object_size = object_size;
    cache->aligned_size = aligned_size;
    cache->align = align;
    cache->first_offset = first_offset;
    cache->objects_per_slab = (PAGE_SIZE - first_offset) / aligned_size;

    uint64_t flags = spinlock_acquire_irqsave(&slab_lock);
    cache->next = cache_chain;
    cache_chain = cache;
    spinlock_release_irqrestore(&slab_lock, flags);

    return cache;
}

void *kmem_cache_alloc(kmem_cache_t *cache) {
    if (!cache) return NULL;

    uint64_t flags = spinlock_acquire_irqsave(&slab_lock);

    slab_page_t *slab = cache->slabs_partial;
    if (!slab) {
        slab = slab_page_create(cache);
        if (!slab) {
            spinlock_release_irqrestore(&slab_lock, flags);
            return NULL;
        }
        cache->slabs_partial = slab;
    }

    void *obj = slab->freelist;
    slab->freelist = *(void **)obj;
    slab->inuse++;
    cache->objects_in_use++;

    if (slab->freelist == NULL) { // Page is now full
        cache->slabs_partial = slab->next;
        slab->next = cache->slabs_full;
        cache->slabs_full = slab;
    }

    spinlock_release_irqrestore(&slab_lock, flags);
    return obj;
}

bool kmem_cache_free(kmem_cache_t *cache, void *obj) {
    if (!cache || !obj) return false;

    uint64_t flags = spinlock_acquire_irqsave(&slab_lock);
    slab_page_t *slab = slab_of(obj);
    bool ok = false;

    // Refuse anything that is not an allocated object of this cache: a
    // pointer from another cache, one that is not at an object boundary, or
    // one that is already free. Each of those would corrupt the free list.
    uint64_t offset = (uint64_t)obj - (uint64_t)slab;
    bool in_page = slab->cache == cache && offset >= cache->first_offset &&
                   (offset - cache->first_offset) % cache->aligned_size == 0 &&
                   (offset - cache->first_offset) / cache->aligned_size < cache->objects_per_slab;
    bool already_free = false;
    if (in_page) {
        for (void *f = slab->freelist; f; f = *(void **)f) {
            if (f == obj) { already_free = true; break; }
        }
    }

    if (in_page && !already_free) {
        bool was_full = (slab->freelist == NULL);
        *(void **)obj = slab->freelist;
        slab->freelist = obj;
        slab->inuse--;
        cache->objects_in_use--;
        ok = true;

        if (was_full) {
            unlink_slab(&cache->slabs_full, slab);
            slab->next = cache->slabs_partial;
            cache->slabs_partial = slab;
        }

        // An empty page goes back to the frame allocator, unless it is the
        // only page with room left. Keeping that one avoids taking and
        // returning a frame every time the object count moves between 0 and 1.
        bool only_page_with_room = (cache->slabs_partial == slab && slab->next == NULL);
        if (slab->inuse == 0 && !only_page_with_room) {
            unlink_slab(&cache->slabs_partial, slab);
            slab->cache = NULL; // A stale pointer into this page will now be refused
            cache->pages--;
            pmm_free_frame((void *)((uint64_t)slab - pmm_get_hhdm_offset()));
        }
    }

    spinlock_release_irqrestore(&slab_lock, flags);
    return ok;
}

void kmem_cache_stats(const kmem_cache_t *cache, size_t *objects_in_use, size_t *pages, size_t *objects_per_page) {
    if (!cache) return;
    if (objects_in_use) *objects_in_use = cache->objects_in_use;
    if (pages) *pages = cache->pages;
    if (objects_per_page) *objects_per_page = cache->objects_per_slab;
}

void kmem_cache_destroy(kmem_cache_t *cache) {
    if (!cache) return;

    uint64_t flags = spinlock_acquire_irqsave(&slab_lock);

    slab_page_t *lists[2] = {cache->slabs_full, cache->slabs_partial};
    for (int i = 0; i < 2; i++) {
        slab_page_t *curr = lists[i];
        while (curr) {
            slab_page_t *next = curr->next;
            curr->cache = NULL;
            pmm_free_frame((void *)((uint64_t)curr - pmm_get_hhdm_offset()));
            curr = next;
        }
    }

    kmem_cache_t **link = &cache_chain;
    while (*link && *link != cache) link = &(*link)->next;
    if (*link) *link = cache->next;

    spinlock_release_irqrestore(&slab_lock, flags);
    kfree(cache);
}
