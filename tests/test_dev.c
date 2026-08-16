#include "test.h"
#include "dev.h"
#include "vfs.h"

TEST(dev_nodes_random_test) {
    dev_init();

    vnode_t *null_node = vfs_find(NULL, "dev/null");
    EXPECT_TRUE(null_node != NULL);

    uint32_t r1 = get_random_u32();
    uint32_t r2 = get_random_u32();
    EXPECT_TRUE(r1 != 0 || r2 != 0);
}

void test_suite_dev(void) {
    kprintf("Suite: Phase 37 Virtual Hardware Devices (/dev)\n");
    RUN_TEST(dev_nodes_random_test);
}
