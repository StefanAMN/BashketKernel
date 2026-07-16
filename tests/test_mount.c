#include "test.h"
#include "mount.h"
#include "vfs.h"

TEST(vfs_mount_table_test) {
    vfs_init();
    vnode_t *root = vfs_get_root();

    EXPECT_TRUE(vfs_mount("/tmp", root) == 0);
    EXPECT_TRUE(vfs_umount("/tmp") == 0);
}

void test_suite_mount(void) {
    kprintf("Suite: Phase 34 VFS Mount Manager\n");
    RUN_TEST(vfs_mount_table_test);
}
