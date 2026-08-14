#include "test.h"
#include "shell.h"
#include "vfs.h"
#include "klog.h"
#include "kstring.h"
#include "heap.h"

// Run `line` in the kernel shell and return what it printed (from the
// kernel's log, which every line of kprintf output reaches), in `out`
static void run_and_capture(const char *line, char *out, size_t size) {
    size_t start = klog_length();
    shell_run_cmd(line);
    size_t n = klog_read(start, out, size - 1);
    out[n] = '\0';
}

// Every command the help lists exists. (Three once did not: removed by
// mistake while "help" went on offering them.)
TEST(every_listed_command_runs) {
    static char out[8192];
    size_t count = 0;
    const shell_command_t *commands = shell_commands(&count);
    EXPECT_TRUE(count >= 15);

    run_and_capture("help", out, sizeof(out));
    for (size_t i = 0; i < count; i++) EXPECT_TRUE(strstr(out, commands[i].name) != NULL);

    // Each one, with an argument where it needs one (the crash command is
    // left out: crashing is what it is for)
    for (size_t i = 0; i < count; i++) {
        const char *name = commands[i].name;
        if (strcmp(name, "crash") == 0) continue;
        char line[64];
        const char *arg = !commands[i].needs_arg ? "" :
                          strcmp(name, "exec") == 0 ? "/bin/hello" :
                          strcmp(name, "kill") == 0 ? "99999" : "/proc/uptime";
        ksnprintf(line, sizeof(line), "%s %s", name, arg);
        if (strcmp(name, "touch") == 0 || strcmp(name, "mkdir") == 0 || strcmp(name, "rm") == 0) continue; // Below
        run_and_capture(line, out, sizeof(out));
        if (strstr(out, "command not found") || strstr(out, "usage:")) kprintf("    '%s' did not run\n", line);
        EXPECT_TRUE(strstr(out, "command not found") == NULL && strstr(out, "usage:") == NULL);
    }
    // (dmesg and mount write to the terminal directly, not through the log:
    // make test-kshell checks what they print)

    run_and_capture("no-such-command", out, sizeof(out));
    EXPECT_TRUE(strstr(out, "command not found") != NULL);
    run_and_capture("cat", out, sizeof(out));
    EXPECT_TRUE(strstr(out, "usage: cat") != NULL);              // One that needs an argument, without
}

// touch, mkdir and rm take paths, on the system disk as in memory
TEST(shell_file_commands) {
    static char out[512];
    run_and_capture("mkdir /tmp/shell-test", out, sizeof(out));
    EXPECT_TRUE(vfs_find(NULL, "/tmp/shell-test") != NULL);
    run_and_capture("touch /tmp/shell-test/empty", out, sizeof(out));
    vnode_t *file = vfs_find(NULL, "/tmp/shell-test/empty");
    EXPECT_TRUE(file != NULL && file->size == 0);
    run_and_capture("rm /tmp/shell-test", out, sizeof(out));
    EXPECT_TRUE(strstr(out, "not empty") != NULL);
    run_and_capture("touch /tmp/shell-test/empty", out, sizeof(out));
    EXPECT_TRUE(strstr(out, "exists already") != NULL);
    run_and_capture("rm /tmp/shell-test/empty", out, sizeof(out));
    run_and_capture("rm /tmp/shell-test/", out, sizeof(out));   // A trailing slash is not part of the name
    EXPECT_TRUE(vfs_find(NULL, "/tmp/shell-test") == NULL);

    // On the system disk (the root): a file made there, then removed
    if (vfs_get_root()->ops) {
        run_and_capture("touch /shell-test-file", out, sizeof(out));
        EXPECT_TRUE(strstr(out, "Created file") != NULL);
        run_and_capture("rm /shell-test-file", out, sizeof(out));
        EXPECT_TRUE(vfs_find(NULL, "/shell-test-file") == NULL);
    }
    run_and_capture("mkdir /no/such/place", out, sizeof(out));
    EXPECT_TRUE(strstr(out, "no such directory") != NULL);
}

void test_suite_shell(void) {
    kprintf("Suite: The kernel shell\n");
    RUN_TEST(every_listed_command_runs);
    RUN_TEST(shell_file_commands);
}
