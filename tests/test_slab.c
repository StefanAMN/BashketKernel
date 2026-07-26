#include "test.h"
#include "slab.h"
#include "pmm.h"
#include "heap.h"

typedef struct {
    int id;
    char data[32];
} test_obj_t;

TEST(slab_allocates_distinct_objects_and_returns_pages) {
    size_t frames_before = pmm_get_free_frames();
    size_t heap_before = kheap_get_allocated_bytes();

    kmem_cache_t *cache = kmem_cache_create("test_cache", sizeof(test_obj_t), 8);
    EXPECT_TRUE(cache != NULL);

    size_t in_use, pages, per_page;
    kmem_cache_stats(cache, &in_use, &pages, &per_page);
    EXPECT_TRUE(per_page >= 90 && per_page <= 101);       // 40-byte objects in a 4 KiB page
    EXPECT_TRUE(in_use == 0 && pages == 0);               // No page until the first allocation

    // Enough objects for three and a half pages; every one distinct and usable
    enum { COUNT = 350 };
    static test_obj_t *objs[COUNT];
    for (int i = 0; i < COUNT; i++) {
        objs[i] = kmem_cache_alloc(cache);
        EXPECT_TRUE(objs[i] != NULL);
        objs[i]->id = i + 1;
        objs[i]->data[31] = (char)i;
    }
    int intact = 1;
    for (int i = 0; i < COUNT; i++) intact &= (objs[i]->id == i + 1 && objs[i]->data[31] == (char)i);
    EXPECT_TRUE(intact);

    kmem_cache_stats(cache, &in_use, &pages, NULL);
    EXPECT_TRUE(in_use == COUNT);
    EXPECT_TRUE(pages == (COUNT + per_page - 1) / per_page);
    EXPECT_TRUE(pmm_get_free_frames() == frames_before - pages);

    // Free every other object: no page becomes empty, so none is returned
    for (int i = 0; i < COUNT; i += 2) EXPECT_TRUE(kmem_cache_free(cache, objs[i]));
    size_t pages_half;
    kmem_cache_stats(cache, &in_use, &pages_half, NULL);
    EXPECT_TRUE(in_use == COUNT / 2 && pages_half == pages);

    // Freed slots are reused before any new page is taken
    for (int i = 0; i < COUNT; i += 2) {
        objs[i] = kmem_cache_alloc(cache);
        objs[i]->id = -1;
    }
    kmem_cache_stats(cache, &in_use, &pages_half, NULL);
    EXPECT_TRUE(in_use == COUNT && pages_half == pages);
    intact = 1;
    for (int i = 1; i < COUNT; i += 2) intact &= (objs[i]->id == i + 1);   // Neighbours untouched
    EXPECT_TRUE(intact);

    // Free everything: the pages go back to the frame allocator, except one
    // that the cache keeps in reserve
    for (int i = 0; i < COUNT; i++) EXPECT_TRUE(kmem_cache_free(cache, objs[i]));
    kmem_cache_stats(cache, &in_use, &pages, NULL);
    EXPECT_TRUE(in_use == 0 && pages == 1);
    EXPECT_TRUE(pmm_get_free_frames() == frames_before - 1);

    // With the reserve page, going between 0 and 1 objects moves no frame
    for (int i = 0; i < 10; i++) {
        void *obj = kmem_cache_alloc(cache);
        EXPECT_TRUE(pmm_get_free_frames() == frames_before - 1);
        kmem_cache_free(cache, obj);
    }

    kmem_cache_destroy(cache);
    EXPECT_TRUE(pmm_get_free_frames() == frames_before);
    EXPECT_TRUE(kheap_get_allocated_bytes() == heap_before);
}

TEST(slab_honours_alignment) {
    size_t aligns[] = { 8, 16, 64, 256, 1024 };
    for (size_t a = 0; a < sizeof(aligns) / sizeof(aligns[0]); a++) {
        kmem_cache_t *cache = kmem_cache_create("aligned", 24, aligns[a]);
        EXPECT_TRUE(cache != NULL);

        void *objs[6];
        int ok = 1;
        for (int i = 0; i < 6; i++) {
            objs[i] = kmem_cache_alloc(cache);
            ok &= (objs[i] != NULL) && (((uint64_t)objs[i] & (aligns[a] - 1)) == 0);
        }
        EXPECT_TRUE(ok);
        for (int i = 0; i < 6; i++) kmem_cache_free(cache, objs[i]);
        kmem_cache_destroy(cache);
    }
}

TEST(slab_rejects_bad_requests) {
    size_t frames_before = pmm_get_free_frames();

    EXPECT_TRUE(kmem_cache_create("zero", 0, 8) == NULL);
    EXPECT_TRUE(kmem_cache_create("odd-align", 32, 24) == NULL);      // Not a power of two
    EXPECT_TRUE(kmem_cache_create("huge-align", 32, 4096) == NULL);
    EXPECT_TRUE(kmem_cache_create("too-big", 4096, 8) == NULL);       // No room next to the header
    EXPECT_TRUE(kmem_cache_create("overflow", (size_t)-4, 8) == NULL);

    // The largest object that does fit: one per page
    kmem_cache_t *big = kmem_cache_create("big", 4000, 8);
    EXPECT_TRUE(big != NULL);
    void *one = kmem_cache_alloc(big), *two = kmem_cache_alloc(big);
    EXPECT_TRUE(one != NULL && two != NULL && one != two);
    EXPECT_TRUE(kmem_cache_free(big, one) && kmem_cache_free(big, two));

    // Frees that would corrupt the cache are refused and change nothing
    kmem_cache_t *a = kmem_cache_create("a", 64, 8);
    kmem_cache_t *b = kmem_cache_create("b", 64, 8);
    uint8_t *obj = kmem_cache_alloc(a);
    void *keep = kmem_cache_alloc(a);                    // Keeps the page alive below
    EXPECT_TRUE(!kmem_cache_free(b, obj));               // Wrong cache
    EXPECT_TRUE(!kmem_cache_free(a, obj + 8));           // Not the start of an object
    EXPECT_TRUE(!kmem_cache_free(a, NULL));
    EXPECT_TRUE(kmem_cache_free(a, obj));
    EXPECT_TRUE(!kmem_cache_free(a, obj));               // Double free
    size_t in_use;
    kmem_cache_stats(a, &in_use, NULL, NULL);
    EXPECT_TRUE(in_use == 1);
    EXPECT_TRUE(kmem_cache_alloc(a) == obj);             // The slot was freed exactly once
    EXPECT_TRUE(kmem_cache_free(a, obj) && kmem_cache_free(a, keep));

    // Destroying a cache that still has objects frees their pages too
    for (int i = 0; i < 200; i++) kmem_cache_alloc(b);
    kmem_cache_destroy(a);
    kmem_cache_destroy(b);
    kmem_cache_destroy(big);
    EXPECT_TRUE(pmm_get_free_frames() == frames_before);
}

void test_suite_slab(void) {
    kprintf("Suite: Slab Allocator\n");
    RUN_TEST(slab_allocates_distinct_objects_and_returns_pages);
    RUN_TEST(slab_honours_alignment);
    RUN_TEST(slab_rejects_bad_requests);
}
