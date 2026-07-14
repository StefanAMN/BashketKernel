#include "test.h"
#include "shell.h"
#include "vfs.h"

TEST(shell_commands) {
    shell_init();

    vfs_init();
    vnode_t *root = vfs_get_root();
    vfs_create_file(root, "readme.txt", "Welcome to BashketOS!", 21);

    kprintf("Testing shell built-in commands:\n");
    shell_run_cmd("help");
    shell_run_cmd("ls");
    shell_run_cmd("cat readme.txt");
    shell_run_cmd("echo Hello Shell Test!");
    shell_run_cmd("mem");
    shell_run_cmd("ps");
    shell_run_cmd("uptime");
    shell_run_cmd("touch newfile.txt");
    shell_run_cmd("mkdir testdir");
    shell_run_cmd("rm newfile.txt");

    EXPECT_TRUE(vfs_find(NULL, "testdir") != NULL);
}

void test_suite_shell(void) {
    kprintf("Suite: Phase 7 Interactive Shell & Coreutils\n");
    RUN_TEST(shell_commands);
}
