/**
 * @file shell.c
 * @brief Implementation of Interactive Kernel Shell and Core Utilities
 */

#include "shell.h"
#include "kprintf.h"
#include "vfs.h"
#include "pmm.h"
#include "heap.h"
#include "sched.h"
#include "pit.h"
#include "process.h"
#include "string_util.h"
#include "rtc.h"
#include "env.h"
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
        kprintf("  env             - Display environment variables\n");
        kprintf("  export K=V      - Set environment variable\n");
        kprintf("  unset K         - Unset environment variable\n");
        kprintf("  dmesg           - Display kernel log ring buffer\n");
        kprintf("  mount           - Display active VFS mount table\n");
        kprintf("  bench           - Execute kernel performance benchmarks\n");
        kprintf("  touch <file>    - Create new empty file in VFS\n");
        kprintf("  mkdir <dir>     - Create new directory in VFS\n");
        kprintf("  rm <file>       - Remove file from VFS\n");
        kprintf("  exec <path>     - Execute ELF binary program\n");
        kprintf("  kill <pid>      - Send SIGKILL signal to process\n");
        kprintf("  clear           - Clear screen\n");
    } else if (strcmp(cmdline, "ls") == 0) {
        vnode_t *root = vfs_get_root();
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

        char buf[256];
        size_t offset = 0;
        size_t read_bytes;
        while ((read_bytes = vfs_read(file, offset, sizeof(buf) - 1, buf)) > 0) {
            buf[read_bytes] = '\0';
            kprintf("%s", buf);
            offset += read_bytes;
        }
        kprintf("\n");
    } else if (strncmp(cmdline, "echo ", 5) == 0) {
        const char *arg = cmdline + 5;
        if (arg[0] == '$') {
            const char *val = env_get(arg + 1);
            if (val) {
                kprintf("%s\n", val);
            } else {
                kprintf("\n");
            }
        } else {
            kprintf("%s\n", arg);
        }
    } else if (strcmp(cmdline, "mem") == 0) {
        kprintf("Physical Memory:\n");
        kprintf("  Total Frames: %zu (%zu MB)\n", pmm_get_total_frames(), (pmm_get_total_frames() * 4096) / (1024 * 1024));
        kprintf("  Free Frames : %zu (%zu MB)\n", pmm_get_free_frames(), (pmm_get_free_frames() * 4096) / (1024 * 1024));
        kprintf("Kernel Heap:\n");
        kprintf("  Allocated Bytes: %zu bytes\n", kheap_get_allocated_bytes());
    } else if (strcmp(cmdline, "ps") == 0) {
        sched_print_tasks();
    } else if (strcmp(cmdline, "uptime") == 0) {
        uint64_t ticks = pit_get_ticks();
        uint64_t seconds = ticks / 100;
        kprintf("Uptime: %zu seconds (%zu ticks)\n", seconds, ticks);
    } else if (strcmp(cmdline, "date") == 0) {
        struct rtc_time t;
        rtc_read_time(&t);
        kprintf("Date (UTC): %d-%02d-%02d %02d:%02d:%02d\n", t.year, t.month, t.day, t.hour, t.minute, t.second);
    } else if (strcmp(cmdline, "env") == 0) {
        env_print_all();
    } else if (strcmp(cmdline, "dmesg") == 0) {
        klog_print_dmesg();
    } else if (strcmp(cmdline, "mount") == 0) {
        vfs_list_mounts();
    } else if (strcmp(cmdline, "bench") == 0) {
        bench_run_all();
    } else if (strncmp(cmdline, "export ", 7) == 0) {
        const char *expr = cmdline + 7;
        char key[64], val[128];
        size_t i = 0;
        while (expr[i] != '\0' && expr[i] != '=' && i < sizeof(key) - 1) {
            key[i] = expr[i];
            i++;
        }
        key[i] = '\0';
        if (expr[i] == '=') {
            size_t j = 0;
            const char *val_src = expr + i + 1;
            while (val_src[j] != '\0' && j < sizeof(val) - 1) {
                val[j] = val_src[j];
                j++;
            }
            val[j] = '\0';
            env_set(key, val);
        }
    } else if (strncmp(cmdline, "unset ", 6) == 0) {
        env_unset(cmdline + 6);
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
        const char *path = cmdline + 5;
        if (process_execve(path) != 0) {
            kprintf("exec: failed to execute '%s'\n", path);
        }
    } else if (strncmp(cmdline, "kill ", 5) == 0) {
        int pid = parse_int(cmdline + 5);
        process_kill(pid, SIGKILL);
    } else if (strcmp(cmdline, "clear") == 0) {
        kprintf("\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n");
    } else {
        kprintf("shell: command not found '%s'. Type 'help' for commands.\n", cmdline);
    }
}

void shell_init(void) {
    kprintf("Interactive Shell Init: Shell system ready.\n");
}
