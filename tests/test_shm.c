#include "test.h"
#include "shm.h"
#include "string_util.h"

TEST(shm_create_get_destroy_test) {
    void *ptr = shm_create(0x1234, 1024);
    EXPECT_TRUE(ptr != NULL);

    void *ptr2 = shm_get(0x1234);
    EXPECT_TRUE(ptr2 == ptr);

    EXPECT_TRUE(shm_destroy(0x1234) == 0);
}

void test_suite_shm(void) {
    kprintf("Suite: Phase 35 POSIX Shared Memory\n");
    RUN_TEST(shm_create_get_destroy_test);
}
