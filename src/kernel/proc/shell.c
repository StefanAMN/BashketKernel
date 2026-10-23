/**
 * @file shell.c
 * @brief The kernel's own shell (see shell.h)
 *
 * Every command is one entry of `commands` below: its name, what it takes,
 * one line of help, and the function that runs it. "help" prints the table
 * and the dispatcher looks names up in it, so the two cannot disagree. (They
 * once did: three commands were deleted by mistake along with something
 * else, and "help" went on offering them for months.)
 */

#include "shell.h"
#include "spinlock.h"
#include "vmm.h"
#include "kprintf.h"
#include "vfs.h"
#include "pmm.h"
#include "heap.h"
#include "sched.h"
#include "pit.h"
#include "process.h"
#include "kstring.h"
#include "walltime.h"
#include "klog.h"
#include "mount.h"
#include "bench.h"
#include "kerrno.h"
#include "file.h"

// Read a decimal number from the start of `str` (stops at the first
// non-digit; 0 if there is none)
static int parse_int(const char *str) {
    int val = 0;
    while (*str >= '0' && *str <= '9') {
        val = val * 10 + (*str - '0');
        str++;
    }
    return val;
}

// --- Commands, one function each. `arg` is the rest of the line after the
// command's name, without leading spaces ("" if there was nothing). --------------

// Deliberate crashes, for checking that each fault is caught and reported.
// noinline + volatile keep the compiler from optimizing the faults away.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winfinite-recursion" // That is the point
// Call itself without end, each frame 512 bytes, until the stack runs out and
// the guard page faults; never returns. The `crash stack` command.
__attribute__((noinline)) static uint64_t crash_recurse(uint64_t depth) {
    volatile uint8_t pad[512];
    pad[0] = (uint8_t)depth;
    return crash_recurse(depth + 1) + pad[0];
}
#pragma GCC diagnostic pop

// crash KIND: cause the fault named KIND in the kernel, to see it reported
static void cmd_crash(const char *kind) {
    if (strcmp(kind, "div0") == 0) {
        volatile int zero = 0;
        volatile int one = 1; // 1 / x with a constant 1 compiles to a compare, not a divide
        volatile int result = one / zero;
        (void)result;
    } else if (strcmp(kind, "pf") == 0) {
        volatile uint64_t *unmapped = (volatile uint64_t *)0xFFFFB00000000000ULL;
        *unmapped = 1;
    } else if (strcmp(kind, "stack") == 0) {
        crash_recurse(0);
    } else if (strcmp(kind, "wtext") == 0) {
        // Kernel code is mapped read-only: writing to it must fault
        volatile uint8_t *code = (volatile uint8_t *)(uintptr_t)shell_run_cmd;
        *code = 0x90;
        kprintf("crash wtext: the write to kernel code was NOT stopped\n");
    } else if (strcmp(kind, "smap") == 0) {
        // Put a user page in front of the kernel and read it without opening
        // a user access window. With SMAP this must fault.
        uint64_t *space = vmm_create_address_space();
        volatile uint8_t *user_page = (volatile uint8_t *)0x400000;
        if (!space || !vmm_alloc_user_page(space, 0x400000, VMM_WRITABLE)) {
            kprintf("crash smap: out of memory\n");
            return;
        }
        uint64_t flags = irq_save();
        vmm_switch_pml4((uint64_t *)((uint64_t)space - pmm_get_hhdm_offset()));
        uint8_t value = *user_page;
        vmm_switch_pml4((uint64_t *)vmm_get_kernel_pml4_phys());
        irq_restore(flags);
        vmm_destroy_address_space(space);
        kprintf("crash smap: the kernel read user memory (value %u) and was NOT stopped\n", value);
    } else {
        kprintf("usage: crash div0|pf|stack|wtext|smap\n");
    }
}

// exec PATH [ARGS]: run a user program in the foreground: split the line into
// arguments, start the process, and wait for it to exit
static void cmd_exec(const char *args_line) {
    char line[128];
    char *argv[PROCESS_MAX_ARGS + 1];
    size_t argc = 0;

    strncpy(line, args_line, sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';

    // Arguments are separated by spaces; "double quotes" keep spaces in one
    for (char *p = line; *p != '\0' && argc < PROCESS_MAX_ARGS; ) {
        while (*p == ' ') p++;
        if (*p == '\0') break;

        char end = ' ';
        if (*p == '"') {
            end = '"';
            p++;
        }
        argv[argc++] = p;
        while (*p != '\0' && *p != end) p++;
        if (*p != '\0') *p++ = '\0';
    }
    argv[argc] = NULL;

    pid_t pid = process_spawn(argv[0], argv);
    if (pid < 0) {
        kprintf("exec: cannot run '%s' (not found, or not a valid executable)\n", argv[0]);
        return;
    }

    int status = 0;
    process_wait(pid, &status);
    if (status != 0) {
        kprintf("[pid %d exited with status %d]\n", pid, status);
    }
}

// ls [DIR]: list a directory (default: the root), one entry per line
static void cmd_ls(const char *path) {
    vnode_t *dir = vfs_find(NULL, path[0] ? path : "/");
    if (!dir || dir->type != VFS_DIRECTORY) {
        kprintf("ls: '%s' is not a directory\n", path);
        return;
    }
    vfs_lock();
    vfs_load_dir(dir);
    kprintf("TYPE  SIZE        NAME\n");
    kprintf("----------------------------------------\n");
    for (vnode_t *child = dir->children; child; child = child->next) {
        const char *type_str = (child->type == VFS_DIRECTORY) ? "[DIR] " : "[FILE]";
        kprintf("%s %zu bytes\t%s\n", type_str, child->size, child->name);
    }
    vfs_unlock();
}

// cat FILE: print a file's contents, read through an open file (which
// keeps it from being removed meanwhile, and a /proc file's text still)
static void cmd_cat(const char *filename) {
    file_t *file = NULL;
    int error = file_open(filename, O_RDONLY, &file);
    if (error == -EISDIR || (error == 0 && file->node->type == VFS_DIRECTORY)) {
        kprintf("cat: '%s' is a directory\n", filename);
        file_close(file);
        return;
    }
    if (error != 0) {
        kprintf("cat: file '%s' not found!\n", filename);
        return;
    }

    // Stop after 64 KiB: /dev/zero and /dev/random never end, and this
    // shell has no Ctrl-C of its own
    char buf[256];
    size_t offset = 0;
    int64_t read_bytes;
    while (offset < 64 * 1024 && (read_bytes = file_read(file, buf, sizeof(buf) - 1)) > 0) {
        buf[read_bytes] = '\0';
        kprintf("%s", buf);
        offset += (size_t)read_bytes;
    }
    file_close(file);
    kprintf("\n");
}

// echo TEXT: print TEXT
static void cmd_echo(const char *text) {
    kprintf("%s\n", text);
}

// mem: frames in use and free, and the kernel heap
static void cmd_mem(const char *arg) {
    (void)arg;
    kprintf("Physical Memory:\n");
    kprintf("  Total Frames: %zu (%zu MB)\n", pmm_get_total_frames(), (pmm_get_total_frames() * 4096) / (1024 * 1024));
    kprintf("  Free Frames : %zu (%zu MB)\n", pmm_get_free_frames(), (pmm_get_free_frames() * 4096) / (1024 * 1024));
    kprintf("Kernel Heap:\n");
    kprintf("  Allocated Bytes: %zu bytes\n", kheap_get_allocated_bytes());
}

// ps: every task, with its state
static void cmd_ps(const char *arg) {
    (void)arg;
    sched_print_tasks();
}

// uptime: time since boot, in seconds and ticks
static void cmd_uptime(const char *arg) {
    (void)arg;
    uint64_t ticks = timer_ticks();
    kprintf("Uptime: %zu seconds (%zu ticks)\n", ticks / TIMER_HZ, ticks);
}

// date: the real-time clock's date and time
static void cmd_date(const char *arg) {
    (void)arg;
    struct rtc_time t;
    walltime_to_date(walltime_seconds(), &t);
    kprintf("Date (UTC): %d-%02d-%02d %02d:%02d:%02d\n", t.year, t.month, t.day, t.hour, t.minute, t.second);
}

// dmesg: the kernel's log since boot
static void cmd_dmesg(const char *arg) {
    (void)arg;
    klog_print_dmesg();
}

// mount: what provides which part of the file tree
static void cmd_mount(const char *arg) {
    (void)arg;
    vfs_list_mounts();
}

// bench: time memory copying and allocation, and print the rates
static void cmd_bench(const char *arg) {
    (void)arg;
    bench_run_all();
}

// What a negative error number from the file layer means, in words
static const char *error_text(int err) {
    switch (-err) {
        case ENOENT:    return "no such file or directory";
        case EEXIST:    return "it exists already";
        case ENOTDIR:   return "not a directory";
        case ENOTEMPTY: return "the directory is not empty";
        case EBUSY:     return "in use";
        case EROFS:     return "read-only";
        case EACCES:    return "permission denied";
        case ENOSPC:    return "no space left";
        case EINVAL:    return "not a valid name";
        case ENAMETOOLONG: return "the name is too long";
        case ENOMEM:    return "out of memory";
        default:        return "failed";
    }
}

// The directory that will hold `path` (a new name in it), or NULL with a
// message if there is none. `leaf` receives the new name.
static vnode_t *parent_for(const char *command, const char *path, char leaf[VFS_NAME_MAX]) {
    vnode_t *parent = vfs_find_parent(path, leaf);
    if (!parent) {
        kprintf("%s: no such directory for '%s'\n", command, path);
        return NULL;
    }
    return parent;
}

// touch FILE: create an empty file
static void cmd_touch(const char *path) {
    char leaf[VFS_NAME_MAX];
    vnode_t *parent = parent_for("touch", path, leaf);
    if (!parent) return;
    int err = vfs_create(parent, leaf, VFS_FILE, NULL);
    if (err == 0) kprintf("Created file '%s'\n", path);
    else kprintf("touch: cannot create '%s': %s\n", path, error_text(err));
}

// mkdir DIR: create a directory
static void cmd_mkdir(const char *path) {
    char leaf[VFS_NAME_MAX];
    vnode_t *parent = parent_for("mkdir", path, leaf);
    if (!parent) return;
    int err = vfs_create(parent, leaf, VFS_DIRECTORY, NULL);
    if (err == 0) kprintf("Created directory '%s'\n", path);
    else kprintf("mkdir: cannot create '%s': %s\n", path, error_text(err));
}

// rm PATH: remove a file or an empty directory
static void cmd_rm(const char *path) {
    char leaf[VFS_NAME_MAX];
    vnode_t *parent = parent_for("rm", path, leaf);
    if (!parent) return;
    int err = vfs_unlink(parent, leaf);
    if (err == 0) kprintf("Removed '%s'\n", path);
    else kprintf("rm: cannot remove '%s': %s\n", path, error_text(err));
}

// kill PID: end a process (SIGKILL)
static void cmd_kill(const char *arg) {
    int pid = parse_int(arg);
    if (process_kill(pid, SIGKILL) != 0) {
        kprintf("kill: no such process %d\n", pid);
    }
}

// clear: blank the screen
static void cmd_clear(const char *arg) {
    (void)arg;
    kprintf("\033[2J\033[H"); // Erase the screen, cursor to the top left
}

static void cmd_help(const char *arg);

static const shell_command_t commands[] = {
    { "help",   "",              "this list",                                     false, cmd_help },
    { "ls",     "[dir]",         "list a directory (the root if none is named)",  false, cmd_ls },
    { "cat",    "<file>",        "print a file",                                  true,  cmd_cat },
    { "echo",   "<text>",        "print text",                                    true,  cmd_echo },
    { "mem",    "",              "memory: frames used and free, the kernel heap", false, cmd_mem },
    { "ps",     "",              "every task and its state",                      false, cmd_ps },
    { "uptime", "",              "time since boot",                               false, cmd_uptime },
    { "date",   "",              "the real-time clock (UTC)",                     false, cmd_date },
    { "dmesg",  "",              "the kernel's log",                              false, cmd_dmesg },
    { "mount",  "",              "what provides which part of the file tree",     false, cmd_mount },
    { "bench",  "",              "time memory copying and allocation",            false, cmd_bench },
    { "touch",  "<file>",        "create an empty file",                          true,  cmd_touch },
    { "mkdir",  "<dir>",         "create a directory",                            true,  cmd_mkdir },
    { "rm",     "<path>",        "remove a file or an empty directory",           true,  cmd_rm },
    { "exec",   "<path> [args]", "run a program and wait for it",                 true,  cmd_exec },
    { "kill",   "<pid>",         "end a process (SIGKILL)",                       true,  cmd_kill },
    { "clear",  "",              "blank the screen",                              false, cmd_clear },
    { "crash",  "<kind>",        "cause a fault: div0, pf, stack, wtext, smap",   true,  cmd_crash },
};
#define COMMAND_COUNT (sizeof(commands) / sizeof(commands[0]))

// help: every command, from the table
static void cmd_help(const char *arg) {
    (void)arg;
    kprintf("The kernel shell's commands:\n");
    for (size_t i = 0; i < COMMAND_COUNT; i++) {
        char usage[32];
        size_t length = ksnprintf(usage, sizeof(usage), "%s %s", commands[i].name, commands[i].args);
        while (length < 21 && length < sizeof(usage) - 1) usage[length++] = ' '; // A column for the help
        usage[length] = '\0';
        kprintf("  %s %s\n", usage, commands[i].help);
    }
}

const shell_command_t *shell_commands(size_t *count) {
    *count = COMMAND_COUNT;
    return commands;
}

void shell_run_cmd(const char *cmdline) {
    if (!cmdline) return;
    while (*cmdline == ' ') cmdline++;
    if (cmdline[0] == '\0') return;

    // The command's name is the first word; the argument is the rest
    size_t name_length = 0;
    while (cmdline[name_length] && cmdline[name_length] != ' ') name_length++;
    const char *arg = cmdline + name_length;
    while (*arg == ' ') arg++;

    for (size_t i = 0; i < COMMAND_COUNT; i++) {
        if (strlen(commands[i].name) != name_length || strncmp(commands[i].name, cmdline, name_length) != 0) continue;
        if (commands[i].needs_arg && arg[0] == '\0') {
            kprintf("usage: %s %s\n", commands[i].name, commands[i].args);
            return;
        }
        commands[i].run(arg);
        return;
    }
    kprintf("shell: command not found '%s'. Type 'help' for commands.\n", cmdline);
}
