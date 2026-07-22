#include "test.h"
#include "process.h"
#include "sched.h"
#include "pmm.h"
#include "heap.h"
#include "spinlock.h"
#include "vfs.h"
#include "string_util.h"

static int run(const char *path, char *const argv[]) {
    pid_t pid = process_spawn(path, argv);
    if (pid < 0) return -1;

    int status = -1;
    if (process_wait(pid, &status) != pid) return -2;
    return status;
}

TEST(spawn_runs_program_and_returns_status) {
    EXPECT_EQ(run("/bin/hello", NULL), 0);

    char *args[] = { "/bin/hello", "alpha", "beta", NULL };
    EXPECT_EQ(run("/bin/hello", args), 0);

    // fault with no argument prints usage and exits 2: the status is real
    EXPECT_EQ(run("/bin/fault", NULL), 2);
}

TEST(spawn_rejects_bad_targets) {
    size_t free_before = pmm_get_free_frames();
    EXPECT_EQ(process_spawn("/bin/no-such-program", NULL), -1);
    EXPECT_EQ(process_spawn("/etc/motd", NULL), -1);   // A file, but not an ELF
    EXPECT_EQ(process_spawn("/bin", NULL), -1);        // A directory
    EXPECT_EQ(process_spawn(NULL, NULL), -1);
    EXPECT_TRUE(pmm_get_free_frames() == free_before); // Failed spawns leak nothing
}

// Each of these does something ring 3 is not allowed to do. The kernel must
// kill the process (status 128 + signal) and keep running.
TEST(faulting_programs_are_killed) {
    char *cli[]    = { "/bin/fault", "cli", NULL };
    char *kread[]  = { "/bin/fault", "kread", NULL };
    char *kwrite[] = { "/bin/fault", "kwrite", NULL };
    char *null[]   = { "/bin/fault", "null", NULL };
    char *text[]   = { "/bin/fault", "text", NULL };
    char *xstack[] = { "/bin/fault", "exec-stack", NULL };
    char *intn[]   = { "/bin/fault", "int", NULL };

    EXPECT_EQ(run("/bin/fault", cli),    128 + SIGSEGV);  // #GP: privileged instruction
    EXPECT_EQ(run("/bin/fault", kread),  128 + SIGSEGV);  // #PF: supervisor page
    EXPECT_EQ(run("/bin/fault", kwrite), 128 + SIGSEGV);
    EXPECT_EQ(run("/bin/fault", null),   128 + SIGSEGV);  // #PF: NULL page
    EXPECT_EQ(run("/bin/fault", text),   128 + SIGSEGV);  // #PF: write to read-only text
    EXPECT_EQ(run("/bin/fault", xstack), 128 + SIGSEGV);  // #PF: execute on NX stack
    EXPECT_EQ(run("/bin/fault", intn),   128 + SIGSEGV);  // #GP: int n with DPL 0 gate
}

TEST(processes_leave_nothing_behind) {
    sched_reap();
    size_t free_before = pmm_get_free_frames();
    size_t heap_before = kheap_get_allocated_bytes();

    for (int i = 0; i < 20; i++) {
        EXPECT_EQ(run("/bin/hello", NULL), 0);
    }

    sched_reap();
    EXPECT_TRUE(pmm_get_free_frames() == free_before);
    EXPECT_TRUE(kheap_get_allocated_bytes() == heap_before);
}

// The user shell builds pipelines and redirections from fork, exec, pipe,
// dup2 and open. Its results are checked here from the kernel side.
TEST(shell_pipelines_and_redirection) {
    sched_reap();
    size_t free_before = pmm_get_free_frames();
    char buf[64] = {0};

    char *to_file[] = { "/bin/sh", "-c", "echo one two three | cat | wc > /tmp/sh_out", NULL };
    EXPECT_EQ(run("/bin/sh", to_file), 0);
    vnode_t *out = vfs_find(NULL, "/tmp/sh_out");
    EXPECT_TRUE(out != NULL);
    if (out) {
        size_t n = vfs_read(out, 0, sizeof(buf) - 1, buf);
        buf[n] = '\0';
        EXPECT_EQ(strcmp(buf, "1 3 14\n"), 0);   // wc: lines words bytes
    }

    // Input redirection and append
    char *append[] = { "/bin/sh", "-c", "cat < /etc/motd >> /tmp/sh_out", NULL };
    EXPECT_EQ(run("/bin/sh", append), 0);
    if (out) {
        size_t n = vfs_read(out, 0, sizeof(buf) - 1, buf);
        buf[n] = '\0';
        EXPECT_EQ(strcmp(buf, "1 3 14\nWelcome to BashketOS.\n"), 0);
    }

    // The status of a pipeline is that of its last program
    char *status[] = { "/bin/sh", "-c", "echo x | fault", NULL };
    EXPECT_EQ(run("/bin/sh", status), 2);
    char *missing[] = { "/bin/sh", "-c", "no-such-program", NULL };
    EXPECT_EQ(run("/bin/sh", missing), 127);
    char *syntax[] = { "/bin/sh", "-c", "echo |", NULL };
    EXPECT_EQ(run("/bin/sh", syntax), 2);
    char *exit_code[] = { "/bin/sh", "-c", "exit 42", NULL };
    EXPECT_EQ(run("/bin/sh", exit_code), 42);

    // A writer whose reader has already exited gets an error, not a hang
    char *broken[] = { "/bin/sh", "-c", "cat /bin/sh | fault", NULL };
    EXPECT_EQ(run("/bin/sh", broken), 2);

    // Every descriptor was closed: the file can be deleted, and nothing leaked
    EXPECT_EQ(vfs_unlink(vfs_find(NULL, "/tmp"), "sh_out"), 0);
    sched_reap();
    EXPECT_TRUE(pmm_get_free_frames() == free_before);
}

// Count tasks in a given state whose name matches
static int count_tasks(const char *name) {
    int n = 0;
    uint64_t flags = irq_save();
    for (task_t *t = sched_first_task(); t; t = t->next) {
        if (strcmp(t->name, name) == 0 && t->state != TASK_ZOMBIE) n++;
    }
    irq_restore(flags);
    return n;
}

TEST(kill_takes_effect_at_a_safe_point) {
    sched_reap();
    size_t free_before = pmm_get_free_frames();

    // A program in an endless loop makes no system calls, so the only place
    // it can die is the timer interrupt's return to user mode.
    pid_t pid = process_spawn("/bin/spin", NULL);
    EXPECT_TRUE(pid > 0);
    sched_sleep_ticks(3);                       // Let it start spinning

    task_t *task = sched_find_task((uint32_t)pid);
    EXPECT_TRUE(task != NULL);
    EXPECT_EQ(process_kill(pid, SIGKILL), 0);
    // The request is recorded; the target has not been torn down from outside
    EXPECT_EQ(task->kill_pending, SIGKILL);
    EXPECT_TRUE(task->state != TASK_ZOMBIE);

    int status = -1;
    EXPECT_EQ(process_wait(pid, &status), pid);
    EXPECT_EQ(status, 128 + SIGKILL);

    // A sleeping process is woken in order to die
    char *sleeper[] = { "/bin/spin", "sleep", NULL };
    pid = process_spawn("/bin/spin", sleeper);
    sched_sleep_ticks(3);
    EXPECT_EQ(sched_find_task((uint32_t)pid)->state, TASK_BLOCKED);
    EXPECT_EQ(process_kill(pid, SIGKILL), 0);
    EXPECT_EQ(process_wait(pid, &status), pid);
    EXPECT_EQ(status, 128 + SIGKILL);

    // Kernel threads and missing processes are refused
    EXPECT_EQ(process_kill(0, SIGKILL), -1);
    EXPECT_EQ(process_kill(pid, SIGKILL), -1);
    EXPECT_EQ(process_kill(99999, SIGKILL), -1);

    EXPECT_TRUE(pmm_get_free_frames() == free_before);
}

// Ctrl-C ends the programs a shell is running but not the shell, which has
// asked to be left alone.
TEST(interrupt_spares_processes_that_ignore_it) {
    sched_reap();
    size_t free_before = pmm_get_free_frames();

    // An interactive shell reading from the console would wait for the test
    // forever, so run one that runs "spin | spin" and then one more command.
    char *args[] = { "/bin/sh", "-c", "spin | spin", NULL };
    pid_t shell = process_spawn("/bin/sh", args);
    EXPECT_TRUE(shell > 0);
    for (int i = 0; i < 100 && count_tasks("spin") < 2; i++) sched_sleep_ticks(1);
    EXPECT_EQ(count_tasks("spin"), 2);

    process_interrupt_all();

    // The shell survived to collect both and report the last one's status
    int status = -1;
    EXPECT_EQ(process_wait(shell, &status), shell);
    EXPECT_EQ(status, 128 + SIGINT);
    EXPECT_EQ(count_tasks("spin"), 0);

    sched_reap();
    EXPECT_TRUE(pmm_get_free_frames() == free_before);
}

void test_suite_process(void) {
    kprintf("Suite: User Processes\n");
    RUN_TEST(spawn_runs_program_and_returns_status);
    RUN_TEST(spawn_rejects_bad_targets);
    RUN_TEST(faulting_programs_are_killed);
    RUN_TEST(processes_leave_nothing_behind);
    RUN_TEST(shell_pipelines_and_redirection);
    RUN_TEST(kill_takes_effect_at_a_safe_point);
    RUN_TEST(interrupt_spares_processes_that_ignore_it);
}
