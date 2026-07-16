/**
 * @file mount.c
 * @brief Implementation of VFS Mount Manager
 */

#include "mount.h"
#include "kprintf.h"
#include "string_util.h"

#define MAX_MOUNTS 16

typedef struct {
    char path[128];
    vnode_t *root_vnode;
} mount_entry_t;

static mount_entry_t mount_table[MAX_MOUNTS];
static size_t mount_count = 0;

int vfs_mount(const char *path, vnode_t *root_vnode) {
    if (!path || !root_vnode || mount_count >= MAX_MOUNTS) return -1;

    size_t i = 0;
    for (; path[i] != '\0' && i < sizeof(mount_table[mount_count].path) - 1; i++) {
        mount_table[mount_count].path[i] = path[i];
    }
    mount_table[mount_count].path[i] = '\0';
    mount_table[mount_count].root_vnode = root_vnode;

    mount_count++;
    return 0;
}

int vfs_umount(const char *path) {
    if (!path) return -1;
    for (size_t i = 0; i < mount_count; i++) {
        if (strcmp(mount_table[i].path, path) == 0) {
            for (size_t j = i; j < mount_count - 1; j++) {
                mount_table[j] = mount_table[j + 1];
            }
            mount_count--;
            return 0;
        }
    }
    return -1;
}

void vfs_list_mounts(void) {
    kprintf("TARGET\t\tTYPE\n");
    kprintf("----------------------------------------\n");
    for (size_t i = 0; i < mount_count; i++) {
        kprintf("%s\t\tvfs\n", mount_table[i].path);
    }
}
