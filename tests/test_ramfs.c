#include "test.h"
#include "ramfs.h"
#include "vfs.h"
#include "string_util.h"

TEST(ramfs_write_read_test) {
    ramfs_init();
    vnode_t *file = vfs_find(NULL, "tmp/scratch.txt");
    EXPECT_TRUE(file != NULL);

    const char *append_data = " - Appended Line";
    size_t written = vfs_write(file, file->size, strlen(append_data), append_data);
    EXPECT_TRUE(written == strlen(append_data));

    char buf[64];
    size_t read_bytes = vfs_read(file, 0, sizeof(buf) - 1, buf);
    buf[read_bytes] = '\0';
    EXPECT_TRUE(read_bytes > 0);
}

void test_suite_ramfs(void) {
    kprintf("Suite: Phase 21 Dynamic RamFS & VFS Write Operations\n");
    RUN_TEST(ramfs_write_read_test);
}
