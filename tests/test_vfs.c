#include "test.h"
#include "vfs.h"
#include "initrd.h"
#include "kstring.h"
#include "file.h"
#include "heap.h"
#include "kerrno.h"

// A file kept in memory, given its contents by pointer, found by a relative
// path and read back. (In /tmp: on the system disk, a directory such as
// /bin keeps its files on the disk, where contents by pointer make no sense.)
TEST(vfs_basic_tree) {
    vnode_t *tmp = vfs_find(NULL, "/tmp");
    EXPECT_TRUE(tmp != NULL);
    if (!tmp) return;

    vnode_t *dir = vfs_create_dir(tmp, "vfs-basic");
    EXPECT_TRUE(dir != NULL);
    if (!dir) return;

    const char *hello_content = "Hello VFS File System!";
    vnode_t *file = vfs_create_file(dir, "hello.txt", hello_content, strlen(hello_content));
    EXPECT_TRUE(file != NULL);

    // Test path lookup
    vnode_t *found = vfs_find(tmp, "vfs-basic/hello.txt");
    EXPECT_TRUE(found != NULL && found == file);
    if (!found) return;
    EXPECT_EQ(found->size, strlen(hello_content));

    // Test reading
    char buf[64] = {0};
    size_t read_bytes = vfs_read(found, 0, sizeof(buf) - 1, buf);
    EXPECT_EQ(read_bytes, strlen(hello_content));
    EXPECT_EQ(strcmp(buf, hello_content), 0);

    EXPECT_EQ(vfs_unlink(dir, "hello.txt"), 0);
    EXPECT_EQ(vfs_unlink(tmp, "vfs-basic"), 0);
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
    char leaf[VFS_NAME_MAX];
    EXPECT_TRUE(vfs_find_parent("/tmp/new.txt", leaf) == vfs_find(NULL, "/tmp"));
    EXPECT_EQ(strcmp(leaf, "new.txt"), 0);
    EXPECT_TRUE(vfs_find_parent("/top", leaf) == vfs_get_root());
    EXPECT_EQ(strcmp(leaf, "top"), 0);
    EXPECT_TRUE(vfs_find_parent("top", leaf) == vfs_get_root());
    EXPECT_TRUE(vfs_find_parent("/no/such/dir/file", leaf) == NULL);
    EXPECT_TRUE(vfs_find_parent("/etc/motd/file", leaf) == NULL);   // Parent is a file
    EXPECT_TRUE(vfs_find_parent("/", leaf) == NULL);
    EXPECT_TRUE(vfs_find_parent("", leaf) == NULL);

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
    vnode_t *found = NULL;
    EXPECT_TRUE(vfs_find_parent_at(bin, "new", &found, leaf) == 0 && found == bin);
    EXPECT_TRUE(vfs_find_parent_at(bin, "../tmp/new", &found, leaf) == 0 && found == vfs_find(NULL, "/tmp"));
    EXPECT_TRUE(vfs_find_parent("/tmp/.", leaf) == NULL);
    EXPECT_TRUE(vfs_find_parent("/tmp/..", leaf) == NULL);
    // The leaf comes without its trailing slashes ("mkdir /tmp/dir/")
    EXPECT_TRUE(vfs_find_parent("/tmp/dir//", leaf) == vfs_find(NULL, "/tmp"));
    EXPECT_EQ(strcmp(leaf, "dir"), 0);

    // Names a new entry cannot have: refused, never cut short or kept with a '/'
    static char long_name[VFS_NAME_MAX + 8];
    memset(long_name, 'n', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';
    vnode_t *tmp = vfs_find(NULL, "/tmp");
    EXPECT_EQ(vfs_create(tmp, long_name, VFS_FILE, NULL), -ENAMETOOLONG);
    EXPECT_EQ(vfs_create(tmp, "a/b", VFS_FILE, NULL), -EINVAL);
    EXPECT_EQ(vfs_create(tmp, "..", VFS_DIRECTORY, NULL), -EINVAL);
    EXPECT_TRUE(vfs_attach(tmp, long_name, VFS_FILE) == NULL);
    char long_path[VFS_NAME_MAX + 16];
    ksnprintf(long_path, sizeof(long_path), "/tmp/%s", long_name);
    EXPECT_EQ(vfs_find_parent_at(NULL, long_path, &found, leaf), -ENAMETOOLONG);
    EXPECT_EQ(vfs_find_parent_at(NULL, "/tmp/..", &found, leaf), -EINVAL);
    EXPECT_EQ(vfs_find_parent_at(NULL, "/no/such/x", &found, leaf), -ENOENT);
    long_name[VFS_NAME_MAX - 1] = '\0';                         // 127 bytes: the longest allowed
    EXPECT_EQ(vfs_create(tmp, long_name, VFS_FILE, NULL), 0);
    EXPECT_TRUE(vfs_find_child(tmp, long_name) != NULL);
    EXPECT_EQ(vfs_unlink(tmp, long_name), 0);

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

// The last entry of an archive may lack its padding: the parser must stop
// at the end of the bytes it was given, not round past it into whatever
// follows (here, bytes that are not a header at all)
TEST(initrd_stops_at_the_end_of_the_archive) {
    static uint8_t archive[4096];
    memset(archive, 0xAA, sizeof(archive));
    tar_header_t *h = (tar_header_t *)archive;
    memset(h, 0, 512);
    memcpy(h->name, "tmp/initrd-test", 15);
    memcpy(h->mode, "0000644", 7);
    memcpy(h->size, "00000001130", 11);   // 600 bytes, octal
    h->typeflag = '0';
    memcpy(h->magic, "ustar", 5);

    EXPECT_TRUE(initrd_mount(archive, 512 + 600));   // Not padded to 1024
    vnode_t *file = vfs_find(NULL, "/tmp/initrd-test");
    EXPECT_TRUE(file != NULL && file->size == 600);
    EXPECT_EQ(vfs_unlink(vfs_find(NULL, "/tmp"), "initrd-test"), 0);

    // A header claiming more than there is is refused
    memcpy(h->size, "00000002000", 11);   // 1024 bytes
    EXPECT_TRUE(!initrd_mount(archive, 512 + 600));
    EXPECT_TRUE(vfs_find(NULL, "/tmp/initrd-test") == NULL);
}

// Symbolic links in the tree kept in memory: followed in the middle of a
// path and at its end, not at its end when asked; relative ones from the
// link's directory; a loop is ELOOP, a link to nothing ENOENT
TEST(symbolic_links_in_memory) {
    vnode_t *tmp = vfs_find(NULL, "/tmp");
    vnode_t *dir = NULL, *file = NULL, *link = NULL;
    EXPECT_EQ(vfs_create(tmp, "links", VFS_DIRECTORY, &dir), 0);
    EXPECT_EQ(vfs_create(dir, "target.txt", VFS_FILE, &file), 0);
    EXPECT_EQ(vfs_symlink(dir, "absolute", "/tmp/links/target.txt", &link), 0);
    EXPECT_TRUE(link != NULL && link->type == VFS_SYMLINK);
    EXPECT_EQ(vfs_symlink(dir, "relative", "target.txt", NULL), 0);
    EXPECT_EQ(vfs_symlink(tmp, "to-dir", "links", NULL), 0);
    EXPECT_EQ(vfs_symlink(dir, "dangling", "nowhere", NULL), 0);
    EXPECT_EQ(vfs_symlink(dir, "loop-a", "loop-b", NULL), 0);
    EXPECT_EQ(vfs_symlink(dir, "loop-b", "loop-a", NULL), 0);

    vnode_t *found = NULL;
    EXPECT_TRUE(vfs_find(NULL, "/tmp/links/absolute") == file);
    EXPECT_TRUE(vfs_find(NULL, "/tmp/links/relative") == file);
    EXPECT_TRUE(vfs_find(NULL, "/tmp/to-dir/relative") == file);           // Through a link to a directory
    EXPECT_TRUE(vfs_find(vfs_find(NULL, "/tmp/to-dir"), "target.txt") == file);
    EXPECT_EQ(vfs_lookup(NULL, "/tmp/links/absolute", false, &found), 0); // Not followed at the end...
    EXPECT_TRUE(found == link);
    EXPECT_EQ(vfs_lookup(NULL, "/tmp/to-dir/absolute", false, &found), 0); // ...but in the middle
    EXPECT_TRUE(found == link);
    EXPECT_EQ(vfs_lookup(NULL, "/tmp/links/dangling", true, &found), -ENOENT);
    EXPECT_EQ(vfs_lookup(NULL, "/tmp/links/loop-a", true, &found), -ELOOP);
    EXPECT_EQ(vfs_lookup(NULL, "/tmp/links/target.txt/x", true, &found), -ENOTDIR);

    char buf[64];
    EXPECT_EQ(vfs_readlink(link, buf, sizeof(buf)), 21);
    EXPECT_TRUE(memcmp(buf, "/tmp/links/target.txt", 21) == 0);
    EXPECT_EQ(vfs_readlink(file, buf, sizeof(buf)), -EINVAL);
    EXPECT_EQ(vfs_symlink(dir, "absolute", "x", NULL), -EEXIST);
    EXPECT_EQ(vfs_symlink(dir, "empty", "", NULL), -ENOENT);
    EXPECT_EQ(vfs_link(file, dir, "hard"), -EPERM);                       // Not in memory

    // Removing a link removes the link, not what it points to
    EXPECT_EQ(vfs_unlink(dir, "absolute"), 0);
    EXPECT_TRUE(vfs_find(NULL, "/tmp/links/target.txt") == file);
    static const char *const names[] = { "relative", "dangling", "loop-a", "loop-b", "target.txt" };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) EXPECT_EQ(vfs_unlink(dir, names[i]), 0);
    EXPECT_EQ(vfs_unlink(tmp, "to-dir"), 0);
    EXPECT_EQ(vfs_unlink(tmp, "links"), 0);
}

void test_suite_vfs(void) {
    kprintf("Suite: Virtual Filesystem and Open Files\n");
    RUN_TEST(vfs_basic_tree);
    RUN_TEST(vfs_write_semantics);
    RUN_TEST(vfs_paths);
    RUN_TEST(open_files_and_descriptors);
    RUN_TEST(initrd_stops_at_the_end_of_the_archive);
    RUN_TEST(symbolic_links_in_memory);
}
