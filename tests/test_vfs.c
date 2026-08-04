#include "test.h"
#include "vfs.h"
#include "initrd.h"
#include "string_util.h"

TEST(vfs_basic_tree) {
    vfs_init();
    vnode_t *root = vfs_get_root();
    EXPECT_TRUE(root != NULL);

    vnode_t *bin_dir = vfs_create_dir(root, "bin");
    EXPECT_TRUE(bin_dir != NULL);

    const char *hello_content = "Hello VFS File System!";
    vnode_t *file = vfs_create_file(bin_dir, "hello.txt", hello_content, strlen(hello_content));
    EXPECT_TRUE(file != NULL);

    // Test path lookup
    vnode_t *found = vfs_find(root, "bin/hello.txt");
    EXPECT_TRUE(found != NULL);
    EXPECT_EQ(found->size, strlen(hello_content));

    // Test reading
    char buf[64] = {0};
    size_t read_bytes = vfs_read(found, 0, sizeof(buf) - 1, buf);
    EXPECT_EQ(read_bytes, strlen(hello_content));
    EXPECT_EQ(strcmp(buf, hello_content), 0);
}

void test_suite_vfs(void) {
    kprintf("Suite: Phase 6 Virtual Filesystem (VFS)\n");
    RUN_TEST(vfs_basic_tree);
}
