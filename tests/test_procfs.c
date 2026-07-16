#include "test.h"
#include "procfs.h"
#include "vfs.h"
#include "string_util.h"

TEST(procfs_virtual_nodes_test) {
    procfs_init();

    vnode_t *version_node = vfs_find(NULL, "proc/version");
    EXPECT_TRUE(version_node != NULL);

    vnode_t *cpuinfo_node = vfs_find(NULL, "proc/cpuinfo");
    EXPECT_TRUE(cpuinfo_node != NULL);

    char buf[64];
    size_t read_bytes = vfs_read(version_node, 0, sizeof(buf) - 1, buf);
    buf[read_bytes] = '\0';

    EXPECT_TRUE(read_bytes > 0);
    EXPECT_TRUE(strncmp(buf, "BashketOS", 9) == 0);
}

void test_suite_procfs(void) {
    kprintf("Suite: Phase 24 Pseudo-Filesystem (/proc)\n");
    RUN_TEST(procfs_virtual_nodes_test);
}
