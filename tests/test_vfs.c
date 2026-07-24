#include "test.h"
#include "vfs.h"
#include "initrd.h"
#include "string_util.h"
#include "file.h"
#include "heap.h"
#include "kerrno.h"

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

TEST(vfs_write_semantics) {
    size_t heap_before = kheap_get_allocated_bytes();
    vnode_t *tmp = vfs_find(NULL, "/tmp");
    EXPECT_TRUE(tmp != NULL);

    // The initial contents are a constant the node does not own
    static const char original[] = "0123456789";
    vnode_t *file = vfs_create_file(tmp, "vfs_write_test", original, 10);
    EXPECT_TRUE(file != NULL);
    EXPECT_TRUE(vfs_create_file(tmp, "vfs_write_test", "", 0) == NULL); // Name is taken

    // Overwriting the middle keeps what comes after it
    EXPECT_EQ(vfs_write(file, 2, 3, "abc"), 3u);
    char buf[64] = {0};
    EXPECT_EQ(vfs_read(file, 0, sizeof(buf), buf), 10u);
    EXPECT_EQ(memcmp(buf, "01abc56789", 10), 0);
    EXPECT_EQ(memcmp(original, "0123456789", 10), 0);   // The constant is untouched
    EXPECT_TRUE(file->owns_data);

    // Writing past the end grows the file; the gap reads as zeros
    EXPECT_EQ(vfs_write(file, 14, 2, "ZZ"), 2u);
    EXPECT_EQ(file->size, 16u);
    EXPECT_EQ(vfs_read(file, 8, sizeof(buf), buf), 8u);
    EXPECT_EQ(memcmp(buf, "89\0\0\0\0ZZ", 8), 0);

    // Many appends: the buffer is reused and grown, not leaked each time
    for (int i = 0; i < 200; i++) {
        EXPECT_EQ(vfs_write(file, file->size, 5, "12345"), 5u);
    }
    EXPECT_EQ(file->size, 1016u);
    EXPECT_EQ(vfs_read(file, 1011, sizeof(buf), buf), 5u);
    EXPECT_EQ(memcmp(buf, "12345", 5), 0);
    EXPECT_EQ(vfs_read(file, 1016, sizeof(buf), buf), 0u);   // At the end
    EXPECT_EQ(vfs_read(file, 5000, sizeof(buf), buf), 0u);   // Past the end

    vfs_truncate(file, 4);
    EXPECT_EQ(file->size, 4u);

    // Removing the file gives back the node and its buffer
    EXPECT_EQ(vfs_unlink(tmp, "vfs_write_test"), 0);
    EXPECT_EQ(vfs_unlink(tmp, "vfs_write_test"), -ENOENT);
    EXPECT_TRUE(vfs_find(NULL, "/tmp/vfs_write_test") == NULL);
    EXPECT_TRUE(kheap_get_allocated_bytes() == heap_before);
}

TEST(vfs_paths) {
    const char *leaf = NULL;
    EXPECT_TRUE(vfs_find_parent("/tmp/new.txt", &leaf) == vfs_find(NULL, "/tmp"));
    EXPECT_EQ(strcmp(leaf, "new.txt"), 0);
    EXPECT_TRUE(vfs_find_parent("/top", &leaf) == vfs_get_root());
    EXPECT_EQ(strcmp(leaf, "top"), 0);
    EXPECT_TRUE(vfs_find_parent("top", &leaf) == vfs_get_root());
    EXPECT_TRUE(vfs_find_parent("/no/such/dir/file", &leaf) == NULL);
    EXPECT_TRUE(vfs_find_parent("/etc/motd/file", &leaf) == NULL);   // Parent is a file
    EXPECT_TRUE(vfs_find_parent("/", &leaf) == NULL);
    EXPECT_TRUE(vfs_find_parent("", &leaf) == NULL);

    // "." and "..", repeated and trailing slashes, relative lookups
    vnode_t *bin = vfs_find(NULL, "/bin");
    vnode_t *hello = vfs_find(NULL, "/bin/hello");
    EXPECT_TRUE(bin != NULL && hello != NULL);
    EXPECT_TRUE(vfs_find(NULL, "/bin/./hello") == hello);
    EXPECT_TRUE(vfs_find(NULL, "/tmp/../bin//hello") == hello);
    EXPECT_TRUE(vfs_find(NULL, "/../../bin") == bin);          // ".." stops at the root
    EXPECT_TRUE(vfs_find(bin, "hello") == hello);
    EXPECT_TRUE(vfs_find(bin, "../bin/hello") == hello);
    EXPECT_TRUE(vfs_find(bin, ".") == bin);
    EXPECT_TRUE(vfs_find(bin, "") == bin);
    EXPECT_TRUE(vfs_find(bin, "/etc") == vfs_find(NULL, "/etc")); // Absolute ignores the base
    EXPECT_TRUE(vfs_find(NULL, "/bin/hello/.") == NULL);        // A file has no "."
    EXPECT_TRUE(vfs_find(NULL, "/bin/hello/..") == NULL);
    EXPECT_TRUE(vfs_find_parent_at(bin, "new", &leaf) == bin);
    EXPECT_TRUE(vfs_find_parent_at(bin, "../tmp/new", &leaf) == vfs_find(NULL, "/tmp"));
    EXPECT_TRUE(vfs_find_parent("/tmp/.", &leaf) == NULL);
    EXPECT_TRUE(vfs_find_parent("/tmp/..", &leaf) == NULL);

    // A node's absolute path
    char path[32];
    EXPECT_EQ(vfs_path_of(hello, path, sizeof(path)), 10u);
    EXPECT_EQ(strcmp(path, "/bin/hello"), 0);
    EXPECT_EQ(vfs_path_of(vfs_get_root(), path, sizeof(path)), 1u);
    EXPECT_EQ(strcmp(path, "/"), 0);
    EXPECT_EQ(vfs_path_of(hello, path, 10), 0u);                // One byte too small

    // A directory with something in it can not be removed
    EXPECT_EQ(vfs_unlink(vfs_get_root(), "bin"), -ENOTEMPTY);
    EXPECT_TRUE(vfs_find(NULL, "/bin/hello") != NULL);
    EXPECT_TRUE(vfs_find(NULL, "/bin/hello/x") == NULL);
    EXPECT_TRUE(vfs_find(NULL, "/bin/") == vfs_find(NULL, "/bin"));
}

TEST(open_files_and_descriptors) {
    size_t heap_before = kheap_get_allocated_bytes();
    file_t *file = NULL;
    char buf[32] = {0};

    EXPECT_EQ(file_open("/tmp/nope", O_RDONLY, &file), -ENOENT);
    EXPECT_EQ(file_open("/nodir/x", O_WRONLY | O_CREAT, &file), -ENOENT);
    EXPECT_EQ(file_open("/tmp", O_WRONLY, &file), -EISDIR);

    // Create, write, and read back through a second open file
    EXPECT_EQ(file_open("/tmp/fd_test", O_WRONLY | O_CREAT, &file), 0);
    EXPECT_EQ(file_write(file, "hello ", 6), 6);
    EXPECT_EQ(file_write(file, "world", 5), 5);          // Continues at the position
    EXPECT_EQ(file_read(file, buf, 4), -EBADF);          // Opened write-only

    file_t *reader = NULL;
    EXPECT_EQ(file_open("/tmp/fd_test", O_RDONLY, &reader), 0);
    EXPECT_EQ(file_read(reader, buf, 6), 6);
    EXPECT_EQ(file_read(reader, buf + 6, sizeof(buf) - 7), 5);
    EXPECT_EQ(strcmp(buf, "hello world"), 0);
    EXPECT_EQ(file_read(reader, buf, 8), 0);             // End of file
    EXPECT_EQ(file_write(reader, "x", 1), -EBADF);       // Opened read-only

    // Seeking
    EXPECT_EQ(file_seek(reader, 6, SEEK_SET), 6);
    EXPECT_EQ(file_read(reader, buf, 5), 5);
    EXPECT_EQ(memcmp(buf, "world", 5), 0);
    EXPECT_EQ(file_seek(reader, -5, SEEK_END), 6);
    EXPECT_EQ(file_seek(reader, -1, SEEK_SET), -EINVAL);
    EXPECT_EQ(file_seek(reader, 0, 99), -EINVAL);

    // A duplicate shares the position; a file stays open until the last close
    file_t *dup = file_dup(reader);
    EXPECT_TRUE(dup == reader && reader->refs == 2);
    file_close(dup);
    EXPECT_EQ(file_seek(reader, 0, SEEK_CUR), 6);

    // An open file can not be deleted
    vnode_t *tmp = vfs_find(NULL, "/tmp");
    EXPECT_EQ(vfs_unlink(tmp, "fd_test"), -EBUSY);
    file_close(file);
    file_close(reader);

    // O_APPEND always writes at the end; O_TRUNC empties the file
    EXPECT_EQ(file_open("/tmp/fd_test", O_WRONLY | O_APPEND, &file), 0);
    EXPECT_EQ(file_write(file, "!", 1), 1);
    file_close(file);
    EXPECT_EQ(vfs_find(NULL, "/tmp/fd_test")->size, 12u);
    EXPECT_EQ(file_open("/tmp/fd_test", O_WRONLY | O_TRUNC, &file), 0);
    EXPECT_EQ(vfs_find(NULL, "/tmp/fd_test")->size, 0u);
    file_close(file);
    EXPECT_EQ(vfs_unlink(tmp, "fd_test"), 0);

    // Directories: readable entry by entry, not as bytes
    EXPECT_EQ(file_open("/bin", O_RDONLY, &file), 0);
    EXPECT_EQ(file_read(file, buf, sizeof(buf)), -EISDIR);
    EXPECT_EQ(file_seek(file, 0, SEEK_SET), -ESPIPE);
    struct dirent_info entry;
    int entries = 0, found_hello = 0;
    while (file_readdir(file, &entry) == 1) {
        entries++;
        if (strcmp(entry.name, "hello") == 0 && entry.type == VFS_FILE && entry.size > 0) found_hello = 1;
    }
    EXPECT_TRUE(entries >= 3 && found_hello);
    EXPECT_EQ(file_readdir(file, &entry), 0);            // Stays at the end
    file_close(file);
    EXPECT_EQ(file_open("/etc/motd", O_RDONLY, &file), 0);
    EXPECT_EQ(file_readdir(file, &entry), -ENOTDIR);
    file_close(file);

    // Descriptor table: lowest free slot first, error when full
    file_t *table[TASK_MAX_FDS] = {0};
    for (int i = 0; i < TASK_MAX_FDS; i++) {
        EXPECT_EQ(fd_install(table, file_open_console()), i);
    }
    file_t *extra = file_open_console();
    EXPECT_EQ(fd_install(table, extra), -EMFILE);
    file_close(extra);
    file_close(table[3]);
    table[3] = NULL;
    EXPECT_EQ(fd_install(table, file_open_console()), 3);
    file_close_all(table);
    EXPECT_TRUE(table[0] == NULL && table[TASK_MAX_FDS - 1] == NULL);

    EXPECT_TRUE(kheap_get_allocated_bytes() == heap_before);
}

void test_suite_vfs(void) {
    kprintf("Suite: Virtual Filesystem and Open Files\n");
    RUN_TEST(vfs_basic_tree);
    RUN_TEST(vfs_write_semantics);
    RUN_TEST(vfs_paths);
    RUN_TEST(open_files_and_descriptors);
}
