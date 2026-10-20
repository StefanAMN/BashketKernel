/**
 * @file heap.c
 * @brief The kernel heap: kmalloc and kfree (see heap.h)
 *
 * The heap is one stretch of kernel address space, grown page by page at
 * its end, divided into blocks that lie one after another. Every block
 * starts with a 16-byte header: its own size and the size of the block
 * before it ("boundary tags"). From any block, then, both neighbours can be
 * found without walking anything, and freeing a block merges it with a
 * free neighbour on either side in constant time.
 *
 * Free blocks are also on one of a set of lists, by size class: list k
 * holds the free blocks of 2^k to 2^(k+1) - 1 bytes. A request looks in
 * the list of its own class for the first block big enough, and failing
 * that takes the first block of any larger class, which is big enough by
 * construction. So neither kmalloc nor kfree depends on how many blocks
 * are in use: the previous allocator walked all of them, in both.
 *
 *     | size | prev_size | payload ...                         |
 *     | size | prev_size | next free | prev free | (unused) ... |  (a free block)
 *
 * Sizes are whole blocks, header included, in multiples of 16, so their
 * lowest bit is free to say whether the block is free.
 */

#include "heap.h"
#include "vmm.h"
#include "pmm.h"
#include "kprintf.h"
#include "spinlock.h"

typedef struct block {
    size_t size;               // The whole block, header included; bit 0 set: free
    size_t prev_size;          // Size of the block just before this one in memory (0: this is the first)
} block_t;

// In the payload of a free block: its place on its size class's list
typedef struct free_links {
    block_t *next, *prev;
} free_links_t;

#define ALIGNMENT 16
#define ALIGN(size) (((size) + (ALIGNMENT - 1)) & ~(size_t)(ALIGNMENT - 1))
#define HEADER sizeof(block_t)
#define FREE_BIT 1
#define MIN_BLOCK (HEADER + sizeof(free_links_t))   // A free block must hold its links
#define CLASSES 48                                  // Size classes: 2^5 up to 2^47 and beyond
#define GROW_MIN (64 * 1024)                        // Grow by at least this, so small requests do not grow page by page

_Static_assert(sizeof(block_t) == ALIGNMENT, "the header keeps payloads 16-byte aligned");

static block_t *first_block = NULL;   // The block at the start of the heap
static block_t *last_block = NULL;    // The block that ends at heap_end_virt
static block_t *free_lists[CLASSES];
static uint64_t heap_end_virt = KHEAP_START_VIRT;
static size_t total_allocated_bytes = 0;
static size_t failed_allocations = 0;

// The free lists and the blocks are walked and edited by every kmalloc and
// kfree, and a timer interrupt can switch tasks in the middle of either.
// Lock order: heap_lock, then the VMM lock, then the PMM lock.
static spinlock_t heap_lock = SPINLOCK_RANKED("heap", LOCK_RANK_HEAP);

// --- Blocks (all of these: caller holds heap_lock) ----------------------------

// A block's size without its free bit
static size_t size_of(const block_t *b) { return b->size & ~(size_t)FREE_BIT; }

// Whether a block is free
static bool is_free(const block_t *b) { return b->size & FREE_BIT; }

// The block after `b` in memory, or NULL if `b` is the last
static block_t *next_block(block_t *b) {
    return b == last_block ? NULL : (block_t *)((uint8_t *)b + size_of(b));
}

// The block before `b` in memory, or NULL if `b` is the first
static block_t *prev_block(block_t *b) {
    return b->prev_size ? (block_t *)((uint8_t *)b - b->prev_size) : NULL;
}

// The free-list links in a free block's payload
static free_links_t *links(block_t *b) { return (free_links_t *)(b + 1); }

// The size class of a block of `size` bytes: the position of its highest set bit
static unsigned class_of(size_t size) {
    unsigned k = 63 - (unsigned)__builtin_clzll(size);
    return k < CLASSES ? k : CLASSES - 1;
}

// Mark `b` free and put it on its class's list
static void insert_free(block_t *b) {
    b->size |= FREE_BIT;
    unsigned k = class_of(size_of(b));
    links(b)->prev = NULL;
    links(b)->next = free_lists[k];
    if (free_lists[k]) links(free_lists[k])->prev = b;
    free_lists[k] = b;
}

// Take a free block off its list (it stays marked free: the caller decides)
static void unlink_free(block_t *b) {
    unsigned k = class_of(size_of(b));
    if (links(b)->prev) links(links(b)->prev)->next = links(b)->next;
    else free_lists[k] = links(b)->next;
    if (links(b)->next) links(links(b)->next)->prev = links(b)->prev;
}

// `b` has a new size: tell the block after it, which keeps that size too
static void update_next_prev_size(block_t *b) {
    block_t *n = next_block(b);
    if (n) n->prev_size = size_of(b);
}

// Cut an in-use block down to `need` bytes if what is left over can be a
// block of its own, and give the rest back as a free block
static void split(block_t *b, size_t need) {
    size_t size = size_of(b);
    if (size < need + MIN_BLOCK) return;
    block_t *rest = (block_t *)((uint8_t *)b + need);
    rest->size = size - need;
    rest->prev_size = need;
    b->size = need;                            // (In use: no free bit)
    if (last_block == b) last_block = rest;
    else update_next_prev_size(rest);
    insert_free(rest);
}

// --- Pages -------------------------------------------------------------------

// Give the pages in [new_end, heap_end_virt) back to the frame allocator.
static void shrink_heap(uint64_t new_end) {
    while (heap_end_virt > new_end) {
        heap_end_virt -= PAGE_SIZE;
        uint64_t phys = vmm_get_phys(NULL, heap_end_virt);
        vmm_unmap_page(NULL, heap_end_virt);
        if (phys) pmm_free_frame((void *)phys);
    }
}

// Grow the heap by whole pages. All or nothing: if memory runs out part way,
// the pages added so far are returned, so a failed large request does not
// leave the heap sitting on the last free frames of the machine.
static bool expand_heap(size_t bytes) {
    size_t page_count = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t old_end = heap_end_virt;

    for (size_t i = 0; i < page_count; i++) {
        void *phys = pmm_alloc_frame();
        if (phys == NULL) {
            shrink_heap(old_end);
            return false;
        }

        if (!vmm_map_page(NULL, heap_end_virt, (uint64_t)phys, VMM_PRESENT | VMM_WRITABLE)) {
            pmm_free_frame(phys);
            shrink_heap(old_end);
            return false;
        }

        heap_end_virt += PAGE_SIZE;
    }

    return true;
}

// Make room at the end of the heap for a block of `need` bytes: grow it and
// add the new pages to the last block (if free) or as a new free block.
// Returns false, changing nothing, if there are not the frames.
static bool grow_for(size_t need) {
    size_t have = is_free(last_block) ? size_of(last_block) : 0;
    size_t more = need - have;
    if (more < GROW_MIN) more = GROW_MIN;
    uint64_t old_end = heap_end_virt;
    if (!expand_heap(more)) return false;
    size_t added = heap_end_virt - old_end;

    if (is_free(last_block)) {
        unlink_free(last_block);
        last_block->size = size_of(last_block) + added;
        insert_free(last_block);
    } else {
        block_t *fresh = (block_t *)old_end;
        fresh->size = added;
        fresh->prev_size = size_of(last_block);
        last_block = fresh;
        insert_free(fresh);
    }
    return true;
}

// --- kmalloc and kfree -------------------------------------------------------

void kheap_init(void) {
    if (!expand_heap(KHEAP_INITIAL_SIZE)) {
        kprintf("KHEAP ERROR: Failed to initialize kernel heap!\n");
        return;
    }

    first_block = last_block = (block_t *)KHEAP_START_VIRT;
    first_block->size = heap_end_virt - KHEAP_START_VIRT;
    first_block->prev_size = 0;
    insert_free(first_block);

    kprintf("KHEAP Init: Virtual Range: %p - %p (Initial %zu KB)\n",
            (void *)KHEAP_START_VIRT, (void *)heap_end_virt, KHEAP_INITIAL_SIZE / 1024);
}

size_t kheap_get_allocated_bytes(void) {
    return total_allocated_bytes;
}

size_t kheap_get_mapped_bytes(void) {
    return (size_t)(heap_end_virt - KHEAP_START_VIRT);
}

size_t kheap_get_failed_allocations(void) {
    return failed_allocations;
}

// A free block of at least `need` bytes, off its list, or NULL. Only the
// list of `need`'s own class has to be searched: every block on a higher
// list is big enough.
static block_t *take_free(size_t need) {
    unsigned k = class_of(need);
    for (block_t *b = free_lists[k]; b; b = links(b)->next) {
        if (size_of(b) >= need) {
            unlink_free(b);
            return b;
        }
    }
    for (unsigned j = k + 1; j < CLASSES; j++) {
        if (free_lists[j]) {
            block_t *b = free_lists[j];
            unlink_free(b);
            return b;
        }
    }
    return NULL;
}

void *kmalloc(size_t size) {
    if (size == 0 || first_block == NULL) return NULL;
    if (size > KHEAP_MAX_ALLOC) return NULL; // Also keeps the size arithmetic from overflowing
    size_t need = ALIGN(size) + HEADER;
    if (need < MIN_BLOCK) need = MIN_BLOCK;

    uint64_t flags = spinlock_acquire_irqsave(&heap_lock);
    block_t *b = take_free(need);
    if (!b && grow_for(need)) b = take_free(need);
    if (!b) {
        failed_allocations++;
        spinlock_release_irqrestore(&heap_lock, flags);
        return NULL;
    }
    b->size = size_of(b);                      // In use
    split(b, need);
    total_allocated_bytes += size_of(b) - HEADER;
    spinlock_release_irqrestore(&heap_lock, flags);
    return b + 1;
}

void kfree(void *ptr) {
    if (ptr == NULL) return;
    uint64_t flags = spinlock_acquire_irqsave(&heap_lock);

    block_t *b = (block_t *)ptr - 1;
    if (is_free(b)) {
        // Freeing twice would put the block on a list twice and corrupt it
        spinlock_release_irqrestore(&heap_lock, flags);
        kprintf("kfree: %p is already free (ignored)\n", ptr);
        return;
    }
    total_allocated_bytes -= size_of(b) - HEADER;

    // Merge with a free neighbour on either side
    block_t *n = next_block(b);
    if (n && is_free(n)) {
        unlink_free(n);
        if (n == last_block) last_block = b;
        b->size = size_of(b) + size_of(n);
    }
    block_t *p = prev_block(b);
    if (p && is_free(p)) {
        unlink_free(p);
        if (b == last_block) last_block = p;
        p->size = size_of(p) + size_of(b);
        b = p;
    }
    b->size = size_of(b);
    update_next_prev_size(b);

    // A large free block at the very end: hand most of it back to the frame
    // allocator, so that memory the kernel freed (a deleted file, exited
    // processes) becomes available to programs again
    if (b == last_block && size_of(b) >= KHEAP_TRIM_THRESHOLD) {
        uint64_t payload = (uint64_t)b + HEADER;
        uint64_t new_end = page_align_up(payload + KHEAP_TRIM_KEEP);
        uint64_t floor = KHEAP_START_VIRT + KHEAP_INITIAL_SIZE; // Never below the initial size
        if (new_end < floor) new_end = floor;
        if (new_end < heap_end_virt) {
            shrink_heap(new_end);
            b->size = heap_end_virt - (uint64_t)b;
        }
    }
    insert_free(b);
    spinlock_release_irqrestore(&heap_lock, flags);
}

bool kheap_check(void) {
    uint64_t flags = spinlock_acquire_irqsave(&heap_lock);
    bool ok = first_block != NULL;
    size_t free_blocks = 0, listed = 0, in_use_bytes = 0;
    size_t prev_size = 0;
    bool prev_free = false;

    // Every block, in memory order: sizes chain up, no two free blocks
    // touch, and the last one ends where the heap does
    for (block_t *b = first_block; ok && b; b = next_block(b)) {
        size_t size = size_of(b);
        ok = size >= MIN_BLOCK && size % ALIGNMENT == 0 && b->prev_size == prev_size &&
             (uint64_t)b + size <= heap_end_virt && !(prev_free && is_free(b));
        if (is_free(b)) free_blocks++;
        else in_use_bytes += size - HEADER;
        prev_size = size;
        prev_free = is_free(b);
        if (b == last_block) ok = ok && (uint64_t)b + size == heap_end_virt;
    }
    // Every list: only free blocks of its class, linked both ways
    for (unsigned k = 0; ok && k < CLASSES; k++) {
        block_t *prev = NULL;
        for (block_t *b = free_lists[k]; ok && b; b = links(b)->next) {
            ok = is_free(b) && class_of(size_of(b)) == k && links(b)->prev == prev;
            prev = b;
            if (++listed > free_blocks) ok = false;   // (Also stops a loop)
        }
    }
    ok = ok && listed == free_blocks && in_use_bytes == total_allocated_bytes;
    spinlock_release_irqrestore(&heap_lock, flags);
    return ok;
}
