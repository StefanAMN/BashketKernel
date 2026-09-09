#include "test.h"
#include "process.h"
#include "vfs.h"
#include "elf.h"
#include "kstring.h"

TEST(process_fork_test) {
    pid_t child = process_fork();
    EXPECT_TRUE(child > 0);
}

TEST(process_execve_wait_test) {
    // Create mock ELF binary in VFS for /bin/sh
    vfs_init();
    vnode_t *root = vfs_get_root();
    vnode_t *bin_dir = vfs_create_dir(root, "bin");
    
    static Elf64_Ehdr sh_elf;
    memset(&sh_elf, 0, sizeof(sh_elf));
    sh_elf.e_ident[0] = 0x7F;
    sh_elf.e_ident[1] = 'E';
    sh_elf.e_ident[2] = 'L';
    sh_elf.e_ident[3] = 'F';
    sh_elf.e_ident[4] = 2; // 64-bit
    sh_elf.e_type = 2;     // Executable
    sh_elf.e_entry = 0x400000;
    sh_elf.e_phnum = 0;
    sh_elf.e_phoff = 0;

    vfs_create_file(bin_dir, "sh", &sh_elf, sizeof(sh_elf));

    EXPECT_EQ(process_execve("/bin/sh"), 0);

    int status = -1;
    pid_t waited = process_waitpid(100, &status);
    EXPECT_EQ(waited, 100);
    EXPECT_EQ(status, 0);

    EXPECT_EQ(process_kill(100, SIGKILL), 0);
}

void test_suite_process(void) {
    kprintf("Suite: Phase 12 UNIX Process Lifecycle & Signals\n");
    RUN_TEST(process_fork_test);
    RUN_TEST(process_execve_wait_test);
}
