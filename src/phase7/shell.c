/**
 * @file shell.c
 * @brief Implementation of Interactive Kernel Shell and Core Utilities
 */

#include "shell.h"
#include "kprintf.h"
#include "vfs.h"
#include "pmm.h"
#include "heap.h"
#include "string_util.h"

void shell_run_cmd(const char *cmdline) {
    if (!cmdline || cmdline[0] == '\0') return;

    if (strcmp(cmdline, "help") == 0) {
        kprintf("BashketOS Shell Built-in Commands:\n");
        kprintf("  help        - Display this help message\n");
        kprintf("  ls          - List files in root directory\n");
        kprintf("  cat <file>  - Display file contents\n");
        kprintf("  echo <text> - Echo text back to screen\n");
        kprintf("  mem         - Display system memory statistics\n");
        kprintf("  clear       - Clear screen (scroll output)\n");
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
        kprintf("%s\n", cmdline + 5);
    } else if (strcmp(cmdline, "mem") == 0) {
        kprintf("Physical Memory:\n");
        kprintf("  Total Frames: %zu (%zu MB)\n", pmm_get_total_frames(), (pmm_get_total_frames() * 4096) / (1024 * 1024));
        kprintf("  Free Frames : %zu (%zu MB)\n", pmm_get_free_frames(), (pmm_get_free_frames() * 4096) / (1024 * 1024));
        kprintf("Kernel Heap:\n");
        kprintf("  Allocated Bytes: %zu bytes\n", kheap_get_allocated_bytes());
    } else if (strcmp(cmdline, "clear") == 0) {
        kprintf("\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n");
    } else {
        kprintf("shell: command not found '%s'. Type 'help' for commands.\n", cmdline);
    }
}

void shell_init(void) {
    kprintf("Interactive Shell Init: Shell system ready.\n");
}
