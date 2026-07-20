/**
 * @file slab.c
 * @brief Implementation of High-Performance Kernel Object Slab Allocator
 */

#include "slab.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "kprintf.h"
#include "string_util.h"
#include "spinlock.h"

typedef struct slab_page {
    void *freelist;             // Linked list of free objects in this page
    size_t inuse;               // Count of currently allocated objects
    struct slab_page *next;     // Link to next slab page in cache
} slab_page_t;

struct kmem_cache {
    char name[32];              // Cache description
    size_t object_size;         // Actual object byte size
    size_t aligned_size;        // Object size rounded up to alignment
    size_t objects_per_slab;    // Capacity per 4 KiB page
    slab_page_t *slabs_full;    // List of fully allocated slab pages
    slab_page_t *slabs_partial; // List of partially allocated slab pages
    slab_page_t *slabs_free;    // List of completely free slab pages
    struct kmem_cache *next;    // Master cache list link
};

static kmem_cache_t *cache_chain = NULL;

// One lock for all caches. Lock order: slab_lock, then the PMM lock.
static spinlock_t slab_lock = SPINLOCK_INIT;

static slab_page_t *slab_page_create(kmem_cache_t *cache) {
    void *phys_frame = pmm_alloc_frame();
    if (!phys_frame) return NULL;

    uint64_t hhdm = pmm_get_hhdm_offset();
    slab_page_t *slab = (slab_page_t *)((uint64_t)phys_frame + hhdm);
    memset(slab, 0, PAGE_SIZE);

    slab->inuse = 0;
    slab->next = NULL;

    // Build freelist chain inside the page
    uint8_t *obj_base = (uint8_t *)slab + sizeof(slab_page_t);
    // Align first object
    uint64_t addr = (uint64_t)obj_base;
    uint64_t align_mask = 7; // Default 8-byte alignment
    if ((addr & align_mask) != 0) {
        addr = (addr + align_mask) & ~align_mask;
    }
    obj_base = (uint8_t *)addr;

    void *prev_obj = NULL;
    size_t count = 0;

    while (obj_base + cache->aligned_size <= (uint8_t *)slab + PAGE_SIZE) {
        *(void **)obj_base = prev_obj;
        prev_obj = obj_base;
        obj_base += cache->aligned_size;
        count++;
    }

    cache->objects_per_slab = count;
    slab->freelist = prev_obj;
    return slab;
}

kmem_cache_t *kmem_cache_create(const char *name, size_t object_size, size_t align) {
    if (object_size == 0) return NULL;
    if (align < 8) align = 8;

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
    cache->aligned_size = (object_size + align - 1) & ~(align - 1);
    if (cache->aligned_size < sizeof(void *)) {
        cache->aligned_size = sizeof(void *);
    }

    uint64_t flags = spinlock_acquire_irqsave(&slab_lock);
    cache->next = cache_chain;
    cache_chain = cache;
    spinlock_release_irqrestore(&slab_lock, flags);

    kprintf("Slab Init: Cache '%s' created (Obj Size: %zu, Aligned: %zu)\n", cache->name, object_size, cache->aligned_size);
    return cache;
}

// Caller must hold slab_lock.
static void *kmem_cache_alloc_locked(kmem_cache_t *cache) {
    slab_page_t *slab = cache->slabs_partial;
    if (!slab) {
        slab = cache->slabs_free;
        if (slab) {
            cache->slabs_free = slab->next;
        } else {
            slab = slab_page_create(cache);
            if (!slab) return NULL;
        }
        slab->next = cache->slabs_partial;
        cache->slabs_partial = slab;
    }

    // Allocate first object from freelist
    void *obj = slab->freelist;
    if (!obj) return NULL;

    slab->freelist = *(void **)obj;
    slab->inuse++;

    // If slab page is now full, move to slabs_full
    if (slab->freelist == NULL) {
        cache->slabs_partial = slab->next;
        slab->next = cache->slabs_full;
        cache->slabs_full = slab;
    }

    return obj;
}

void *kmem_cache_alloc(kmem_cache_t *cache) {
    if (!cache) return NULL;

    uint64_t flags = spinlock_acquire_irqsave(&slab_lock);
    void *obj = kmem_cache_alloc_locked(cache);
    spinlock_release_irqrestore(&slab_lock, flags);
    return obj;
}

void kmem_cache_free(kmem_cache_t *cache, void *obj) {
    if (!cache || !obj) return;

    uint64_t flags = spinlock_acquire_irqsave(&slab_lock);

    // Locate slab page header from object virtual address (4 KiB page aligned)
    uint64_t page_addr = (uint64_t)obj & ~0xFFFULL;
    slab_page_t *slab = (slab_page_t *)page_addr;

    // Return object to slab freelist
    *(void **)obj = slab->freelist;
    slab->freelist = obj;
    slab->inuse--;

    // Move slab from slabs_full to slabs_partial if it was full
    if (slab->inuse == cache->objects_per_slab - 1) {
        // Unlink from slabs_full
        slab_page_t **curr = &cache->slabs_full;
        while (*curr && *curr != slab) {
            curr = &(*curr)->next;
        }
        if (*curr) {
            *curr = slab->next;
            slab->next = cache->slabs_partial;
            cache->slabs_partial = slab;
        }
    }

    spinlock_release_irqrestore(&slab_lock, flags);
}

void kmem_cache_destroy(kmem_cache_t *cache) {
    if (!cache) return;

    uint64_t flags = spinlock_acquire_irqsave(&slab_lock);

    // Free all slab pages
    slab_page_t *lists[3] = {cache->slabs_full, cache->slabs_partial, cache->slabs_free};
    for (int i = 0; i < 3; i++) {
        slab_page_t *curr = lists[i];
        while (curr) {
            slab_page_t *next = curr->next;
            uint64_t hhdm = pmm_get_hhdm_offset();
            void *phys_frame = (void *)((uint64_t)curr - hhdm);
            pmm_free_frame(phys_frame);
            curr = next;
        }
    }

    // Unlink cache from chain
    kmem_cache_t **curr_c = &cache_chain;
    while (*curr_c && *curr_c != cache) {
        curr_c = &(*curr_c)->next;
    }
    if (*curr_c) {
        *curr_c = cache->next;
    }

    spinlock_release_irqrestore(&slab_lock, flags);

    kfree(cache);
}
