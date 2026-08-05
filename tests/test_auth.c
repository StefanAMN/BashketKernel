#include "test.h"
#include "process.h"
#include "auth.h"
#include "file.h"
#include "vfs.h"
#include "sched.h"
#include "kerrno.h"

TEST(permission_rules) {
    vnode_t file = { .uid = 1000, .mode = 0644 };   // Owner reads and writes, others read
    vnode_t dir = { .uid = 0, .mode = 0666 };       // A shared directory, like /tmp

    // Root may do anything, whatever the bits say
    vnode_t locked = { .uid = 1000, .mode = 0 };
    EXPECT_TRUE(auth_may_access(UID_ROOT, &locked, false) && auth_may_access(UID_ROOT, &locked, true));

    // The owner is judged by the owner bits, everyone else by the others'
    EXPECT_TRUE(auth_may_access(1000, &file, false) && auth_may_access(1000, &file, true));
    EXPECT_TRUE(auth_may_access(1001, &file, false));
    EXPECT_TRUE(!auth_may_access(1001, &file, true));
    file.mode = 0600;
    EXPECT_TRUE(!auth_may_access(1001, &file, false));
    file.mode = 0006;                               // Open to others, closed to its own owner
    EXPECT_TRUE(!auth_may_access(1000, &file, false) && auth_may_access(1001, &file, true));
    EXPECT_TRUE(!auth_may_access(1000, NULL, false));

    // Removing needs a writable directory and ownership of the file (or of
    // the directory): sharing /tmp does not mean deleting each other's files
    file.mode = 0644;
    EXPECT_TRUE(auth_may_remove(1000, &dir, &file));
    EXPECT_TRUE(!auth_may_remove(1001, &dir, &file));
    EXPECT_TRUE(auth_may_remove(UID_ROOT, &dir, &file));
    vnode_t private_dir = { .uid = 1001, .mode = 0600 };
    EXPECT_TRUE(auth_may_remove(1001, &private_dir, &file));   // Its directory's owner may
    EXPECT_TRUE(!auth_may_remove(1000, &private_dir, &file));  // No write access to the directory
    dir.mode = 0644;
    EXPECT_TRUE(!auth_may_remove(1000, &dir, &file));

    // Signals: to your own processes, or from root
    EXPECT_TRUE(auth_may_signal(1000, 1000) && auth_may_signal(UID_ROOT, 1000));
    EXPECT_TRUE(!auth_may_signal(1000, 1001) && !auth_may_signal(1000, UID_ROOT));

    EXPECT_EQ(auth_current_uid(), (uint32_t)UID_ROOT);         // Kernel threads are root
}

TEST(open_respects_permissions) {
    file_t *file = NULL;

    // A user's file in /tmp: created with that owner
    EXPECT_EQ(file_open_as(1000, NULL, "/tmp/owned.txt", O_WRONLY | O_CREAT, &file), 0);
    EXPECT_EQ(file_write(file, "secret", 6), 6);
    file_close(file);
    vnode_t *node = vfs_find(NULL, "/tmp/owned.txt");
    EXPECT_TRUE(node != NULL && node->uid == 1000 && node->mode == 0644);

    // Others may read it but not write, truncate or replace it
    EXPECT_EQ(file_open_as(1001, NULL, "/tmp/owned.txt", O_RDONLY, &file), 0);
    file_close(file);
    EXPECT_EQ(file_open_as(1001, NULL, "/tmp/owned.txt", O_WRONLY, &file), -EACCES);
    EXPECT_EQ(file_open_as(1001, NULL, "/tmp/owned.txt", O_RDWR, &file), -EACCES);
    EXPECT_EQ(file_open_as(1001, NULL, "/tmp/owned.txt", O_WRONLY | O_TRUNC, &file), -EACCES);
    EXPECT_EQ(node->size, 6u);                                  // The refused truncation did not happen

    // Made private, others cannot read it either; the owner and root still can
    node->mode = 0600;
    EXPECT_EQ(file_open_as(1001, NULL, "/tmp/owned.txt", O_RDONLY, &file), -EACCES);
    EXPECT_EQ(file_open_as(1000, NULL, "/tmp/owned.txt", O_RDWR, &file), 0);
    file_close(file);
    EXPECT_EQ(file_open_as(UID_ROOT, NULL, "/tmp/owned.txt", O_RDWR, &file), 0);
    file_close(file);

    // Creating needs a writable directory: / and /bin are root's
    EXPECT_EQ(file_open_as(1000, NULL, "/new.txt", O_WRONLY | O_CREAT, &file), -EACCES);
    EXPECT_EQ(file_open_as(1000, NULL, "/bin/new", O_WRONLY | O_CREAT, &file), -EACCES);
    EXPECT_TRUE(vfs_find(NULL, "/new.txt") == NULL && vfs_find(NULL, "/bin/new") == NULL);

    // System files as shipped: readable programs, a private password file,
    // a raw disk for root only
    EXPECT_EQ(file_open_as(1000, NULL, "/bin/hello", O_RDONLY, &file), 0);
    file_close(file);
    EXPECT_EQ(file_open_as(1000, NULL, "/bin/hello", O_WRONLY, &file), -EACCES);
    EXPECT_EQ(file_open_as(1000, NULL, "/etc/passwd", O_RDONLY, &file), -EACCES);
    EXPECT_EQ(file_open_as(1000, NULL, "/dev/vda", O_RDONLY, &file), -EACCES);
    EXPECT_EQ(file_open_as(1000, NULL, "/dev/null", O_WRONLY, &file), 0);
    file_close(file);
    EXPECT_EQ(file_open_as(1000, NULL, "/disk/readme.txt", O_WRONLY, &file), -EACCES);
    EXPECT_EQ(file_open_as(1000, NULL, "/disk/mine.txt", O_WRONLY | O_CREAT, &file), -EACCES);

    EXPECT_EQ(vfs_unlink(vfs_find(NULL, "/tmp"), "owned.txt"), 0);
}

// Owner, group and others each have a digit; exactly one applies
TEST(group_and_execute_bits) {
    vnode_t file = { .type = VFS_FILE, .uid = 1000, .gid = 100, .mode = 0750 };

    // The owner: rwx
    EXPECT_TRUE(auth_permits(1000, 100, &file, PERM_READ) && auth_permits(1000, 100, &file, PERM_WRITE) &&
                auth_permits(1000, 100, &file, PERM_EXEC));
    // Someone else in the file's group: r-x
    EXPECT_TRUE(auth_permits(1001, 100, &file, PERM_READ) && auth_permits(1001, 100, &file, PERM_EXEC));
    EXPECT_TRUE(!auth_permits(1001, 100, &file, PERM_WRITE));
    // Everyone else, and a process in no group at all: ---
    EXPECT_TRUE(!auth_permits(1001, 200, &file, PERM_READ) && !auth_permits(1001, 200, &file, PERM_EXEC));
    EXPECT_TRUE(!auth_permits(1001, GID_NONE, &file, PERM_READ));

    // Only one digit applies: the owner is not helped by the group's or
    // the others' rights, nor a group member by the others'
    file.mode = 0077;
    EXPECT_TRUE(!auth_permits(1000, 100, &file, PERM_READ));
    file.mode = 0707;
    EXPECT_TRUE(!auth_permits(1001, 100, &file, PERM_READ) && auth_permits(1001, 200, &file, PERM_READ));

    // Root reads and writes anything, but runs only what somebody may run
    file.mode = 0000;
    EXPECT_TRUE(auth_permits(UID_ROOT, 0, &file, PERM_READ) && auth_permits(UID_ROOT, 0, &file, PERM_WRITE));
    EXPECT_TRUE(!auth_permits(UID_ROOT, 0, &file, PERM_EXEC));
    file.mode = 0001;
    EXPECT_TRUE(auth_permits(UID_ROOT, 0, &file, PERM_EXEC));
    EXPECT_TRUE(!auth_permits(1000, 100, NULL, PERM_READ));

    // What the system starts with: programs can be run by all and changed
    // by root only; two of them are set-uid; data files are not programs
    vnode_t *hello = vfs_find(NULL, "/bin/hello");
    vnode_t *su = vfs_find(NULL, "/bin/su");
    vnode_t *passwd = vfs_find(NULL, "/bin/passwd");
    vnode_t *motd = vfs_find(NULL, "/etc/motd");
    vnode_t *accounts = vfs_find(NULL, "/etc/passwd");
    vnode_t *tmp = vfs_find(NULL, "/tmp");
    EXPECT_TRUE(hello && su && passwd && motd && accounts && tmp);
    EXPECT_TRUE(hello->mode == 0755 && hello->uid == UID_ROOT);
    EXPECT_TRUE(su->mode == 04755 && passwd->mode == 04755 && su->uid == UID_ROOT);
    EXPECT_TRUE(motd->mode == 0644 && accounts->mode == 0600);
    EXPECT_TRUE(tmp->mode == 0777 && vfs_get_root()->mode == 0755);

    // A file that is not marked executable is not run, even by the kernel
    EXPECT_EQ(process_spawn("/etc/motd", NULL), -1);
    EXPECT_EQ(process_spawn("/etc/passwd", NULL), -1);
}

// Writing to a set-uid file takes the bit away, unless root is the writer
TEST(setuid_bit_does_not_survive_a_rewrite) {
    file_t *file = NULL;
    EXPECT_EQ(file_open_as(1000, NULL, "/tmp/tool", O_WRONLY | O_CREAT, &file), 0);
    file_close(file);
    vnode_t *node = vfs_find(NULL, "/tmp/tool");
    EXPECT_TRUE(node != NULL && node->uid == 1000 && node->mode == 0644);   // New file: 0666 less the umask

    node->mode = 04755;
    EXPECT_EQ(file_open_as(1000, NULL, "/tmp/tool", O_RDONLY, &file), 0);   // Reading leaves it
    file_close(file);
    EXPECT_TRUE(node->mode == 04755);
    EXPECT_EQ(file_open_as(UID_ROOT, NULL, "/tmp/tool", O_WRONLY, &file), 0); // So does root writing
    file_close(file);
    EXPECT_TRUE(node->mode == 04755);
    EXPECT_EQ(file_open_as(1000, NULL, "/tmp/tool", O_WRONLY, &file), 0);   // Its owner writing does not
    file_close(file);
    EXPECT_TRUE(node->mode == 0755);

    EXPECT_EQ(vfs_unlink(vfs_find(NULL, "/tmp"), "tool"), 0);
}

void test_suite_auth(void) {
    kprintf("Suite: Users and Permissions\n");
    RUN_TEST(permission_rules);
    RUN_TEST(open_respects_permissions);
    RUN_TEST(group_and_execute_bits);
    RUN_TEST(setuid_bit_does_not_survive_a_rewrite);
}
