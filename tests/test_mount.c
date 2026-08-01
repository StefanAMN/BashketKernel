#include "test.h"
#include "mount.h"
#include "file.h"
#include "vfs.h"
#include "kstring.h"

TEST(mount_table_reflects_the_tree) {
    static char text[1024];
    size_t len = mount_describe(text, sizeof(text));
    EXPECT_TRUE(len > 0 && len == strlen(text));

    // Every source that attached part of the tree is listed where it is
    EXPECT_TRUE(strstr(text, "memory / ramfs\n") != NULL);
    EXPECT_TRUE(strstr(text, "boot-module /bin initrd\n") != NULL);
    EXPECT_TRUE(strstr(text, "kernel /proc proc\n") != NULL);
    EXPECT_TRUE(strstr(text, "kernel /dev dev\n") != NULL);
    EXPECT_TRUE(strstr(text, "/dev/vda /disk fat32\n") != NULL);

    // ... and each listed path really is a directory in the tree
    static const char *const paths[] = { "/", "/bin", "/proc", "/dev", "/disk" };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        vnode_t *node = vfs_find(NULL, paths[i]);
        EXPECT_TRUE(node != NULL && node->type == VFS_DIRECTORY);
    }

    // The same text is what /proc/mounts gives a program
    static char from_file[1024];
    file_t *file = NULL;
    EXPECT_EQ(file_open("/proc/mounts", O_RDONLY, &file), 0);
    int64_t n = file_read(file, from_file, sizeof(from_file) - 1);
    file_close(file);
    EXPECT_EQ(n, (int64_t)len);
    EXPECT_EQ(memcmp(from_file, text, len), 0);
}

TEST(mount_table_rules) {
    size_t before = mount_count();

    EXPECT_EQ(mount_register("/proc", "proc", "again"), -1);     // Already registered
    EXPECT_EQ(mount_register(NULL, "x", "y"), -1);
    EXPECT_EQ(mount_register("/x", NULL, "y"), -1);
    EXPECT_TRUE(mount_count() == before);

    // A buffer too small gets a truncated but terminated table
    char small[16];
    size_t len = mount_describe(small, sizeof(small));
    EXPECT_TRUE(len < sizeof(small) && small[len] == '\0');

    // The table has a fixed size; filling it is refused cleanly
    static const char *const names[MOUNT_MAX] = { "/m0", "/m1", "/m2", "/m3", "/m4", "/m5", "/m6", "/m7",
                                                  "/m8", "/m9", "/m10", "/m11", "/m12", "/m13", "/m14", "/m15" };
    size_t added = 0;
    while (added < MOUNT_MAX && mount_register(names[added], "test", "test") == 0) added++;
    EXPECT_TRUE(mount_count() == MOUNT_MAX);
    EXPECT_TRUE(added == MOUNT_MAX - before);
    EXPECT_EQ(mount_register("/one-too-many", "test", "test"), -1);
}

void test_suite_mount(void) {
    kprintf("Suite: Mount Table\n");
    RUN_TEST(mount_table_reflects_the_tree);
    RUN_TEST(mount_table_rules);
}
