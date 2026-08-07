/**
 * @file shell.c
 * @brief Implementation of Interactive Kernel Shell and Core Utilities
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
#include "string_util.h"
#include "rtc.h"
#include "klog.h"
#include "mount.h"
#include "bench.h"
#include "crypto.h"

static int parse_int(const char *str) {
    int val = 0;
    while (*str >= '0' && *str <= '9') {
        val = val * 10 + (*str - '0');
        str++;
    }
    return val;
}

// Deliberate crashes, for checking that each fault is caught and reported.
// noinline + volatile keep the compiler from optimizing the faults away.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winfinite-recursion" // That is the point
__attribute__((noinline)) static uint64_t crash_recurse(uint64_t depth) {
    volatile uint8_t pad[512];
    pad[0] = (uint8_t)depth;
    return crash_recurse(depth + 1) + pad[0];
}
#pragma GCC diagnostic pop

static void shell_crash(const char *kind) {
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

// Run a user program in the foreground: split the line into arguments,
// start the process, and wait for it to exit.
static void shell_exec(const char *args_line) {
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

    if (argc == 0) {
        kprintf("usage: exec <path> [args...]\n");
        return;
    }

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

void shell_run_cmd(const char *cmdline) {
    if (!cmdline || cmdline[0] == '\0') return;

    if (strcmp(cmdline, "help") == 0) {
        kprintf("BashketOS Shell Core Utilities:\n");
        kprintf("  help            - Display this help message\n");
        kprintf("  ls              - List files in root directory\n");
        kprintf("  cat <file>      - Display file contents\n");
        kprintf("  echo <text>     - Echo text back to screen\n");
        kprintf("  mem             - Display system memory statistics\n");
        kprintf("  ps              - List active processes and tasks\n");
        kprintf("  uptime          - Display system uptime and timer ticks\n");
        kprintf("  date            - Display real-time clock UTC timestamp\n");
        kprintf("  dmesg           - Display kernel log ring buffer\n");
        kprintf("  mount           - Display active VFS mount table\n");
        kprintf("  bench           - Execute kernel performance benchmarks\n");
        kprintf("  touch <file>    - Create new empty file in VFS\n");
        kprintf("  mkdir <dir>     - Create new directory in VFS\n");
        kprintf("  rm <file>       - Remove file from VFS\n");
        kprintf("  exec <path> [args] - Run a user program and wait for it\n");
        kprintf("  kill <pid>      - Send SIGKILL signal to process\n");
        kprintf("  clear           - Clear screen\n");
        kprintf("  crash <kind>    - Trigger a fault on purpose (div0, pf, stack, wtext, smap)\n");
    } else if (strcmp(cmdline, "ls") == 0) {
        vnode_t *root = vfs_get_root();
        vfs_lock();
        vfs_load_dir(root);
        vfs_unlock();
        if (!root || !root->children) {
            kprintf("(empty directory)\n");
            return;
        }

        vnode_t *child = root->children;
        kprintf("TYPE  SIZE        NAME\n");
        kprintf("----------------------------------------\n");
        while (child) {
            const char *type_str = (child->type == VFS_DIRECTORY) ? "[DIR] " : "[FILE]";
            kprintf("%s %zu bytes\t%s\n", type_str, child->size, child->name);
            child = child->next;
        }
    } else if (strncmp(cmdline, "cat ", 4) == 0) {
        const char *filename = cmdline + 4;
        vnode_t *file = vfs_find(NULL, filename);
        if (!file) {
            kprintf("cat: file '%s' not found!\n", filename);
            return;
        }
        if (file->type == VFS_DIRECTORY) {
            kprintf("cat: '%s' is a directory\n", filename);
            return;
        }

        // Stop after 64 KiB: /dev/zero and /dev/random never end, and this
        // shell has no Ctrl-C of its own
        char buf[256];
        size_t offset = 0;
        size_t read_bytes;
        while (offset < 64 * 1024 && (read_bytes = vfs_read(file, offset, sizeof(buf) - 1, buf)) > 0) {
            buf[read_bytes] = '\0';
            kprintf("%s", buf);
            offset += read_bytes;
        }
        kprintf("\n");
    } else if (strncmp(cmdline, "echo ", 5) == 0) {
        kprintf("%s\n", cmdline + 5);
    } else if (strcmp(cmdline, "mem") == 0) {
        kprintf("Physical Memory:\n");
        kprintf("  Total Frames: %zu (%zu MB)\n", pmm_get_total_frames(), (pmm_get_total_frames() * 4096) / (1024 * 1024));
        kprintf("  Free Frames : %zu (%zu MB)\n", pmm_get_free_frames(), (pmm_get_free_frames() * 4096) / (1024 * 1024));
        kprintf("Kernel Heap:\n");
        kprintf("  Allocated Bytes: %zu bytes\n", kheap_get_allocated_bytes());
    } else if (strcmp(cmdline, "ps") == 0) {
        sched_print_tasks();
    } else if (strcmp(cmdline, "uptime") == 0) {
        uint64_t ticks = timer_ticks();
        uint64_t seconds = ticks / TIMER_HZ;
        kprintf("Uptime: %zu seconds (%zu ticks)\n", seconds, ticks);
    } else if (strcmp(cmdline, "date") == 0) {
        struct rtc_time t;
        rtc_read_time(&t);
        kprintf("Date (UTC): %d-%02d-%02d %02d:%02d:%02d\n", t.year, t.month, t.day, t.hour, t.minute, t.second);
    } else if (strncmp(cmdline, "touch ", 6) == 0) {
        const char *filename = cmdline + 6;
        vnode_t *file = vfs_create_file(NULL, filename, "", 0);
        if (file) {
            kprintf("Created file '%s'\n", filename);
        } else {
            kprintf("touch: failed to create file '%s'\n", filename);
        }
    } else if (strncmp(cmdline, "mkdir ", 6) == 0) {
        const char *dirname = cmdline + 6;
        vnode_t *dir = vfs_create_dir(NULL, dirname);
        if (dir) {
            kprintf("Created directory '%s'\n", dirname);
        } else {
            kprintf("mkdir: failed to create directory '%s'\n", dirname);
        }
    } else if (strncmp(cmdline, "rm ", 3) == 0) {
        const char *filename = cmdline + 3;
        if (vfs_unlink(NULL, filename) == 0) {
            kprintf("Removed '%s'\n", filename);
        } else {
            kprintf("rm: file '%s' not found\n", filename);
        }
    } else if (strncmp(cmdline, "exec ", 5) == 0) {
        shell_exec(cmdline + 5);
    } else if (strncmp(cmdline, "kill ", 5) == 0) {
        int pid = parse_int(cmdline + 5);
        if (process_kill(pid, SIGKILL) != 0) {
            kprintf("kill: no such process %d\n", pid);
        }
    } else if (strncmp(cmdline, "crash ", 6) == 0) {
        shell_crash(cmdline + 6);
    } else if (strcmp(cmdline, "clear") == 0) {
        kprintf("\033[2J\033[H"); // Erase the screen, cursor to the top left
    } else {
        kprintf("shell: command not found '%s'. Type 'help' for commands.\n", cmdline);
    }
}

void shell_init(void) {
    kprintf("Interactive Shell Init: Shell system ready.\n");
}
