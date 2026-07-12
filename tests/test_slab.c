#include "test.h"
#include "slab.h"
#include "cow.h"

typedef struct {
    int id;
    char data[32];
} test_obj_t;

TEST(slab_allocator_test) {
    kmem_cache_t *cache = kmem_cache_create("test_cache", sizeof(test_obj_t), 8);
    EXPECT_TRUE(cache != NULL);

    test_obj_t *objs[100];
    for (int i = 0; i < 100; i++) {
        objs[i] = (test_obj_t *)kmem_cache_alloc(cache);
        EXPECT_TRUE(objs[i] != NULL);
        objs[i]->id = i + 1;
    }

    for (int i = 0; i < 100; i++) {
        EXPECT_EQ(objs[i]->id, i + 1);
        kmem_cache_free(cache, objs[i]);
    }

    kmem_cache_destroy(cache);
}

void test_suite_slab(void) {
    kprintf("Suite: Phase 8 Slab Allocator & Refcounting\n");
    RUN_TEST(slab_allocator_test);
}
