/**
 * @file ramfs.c
 * @brief Implementation of Dynamic RAM Filesystem
 */

#include "ramfs.h"
#include "vfs.h"
#include "kprintf.h"

void ramfs_init(void) {
    vfs_init();
    vnode_t *root = vfs_get_root();
    vnode_t *tmp_dir = vfs_create_dir(root, "tmp");
    if (tmp_dir) tmp_dir->mode = 0666; // Everyone may create files here (and remove their own)

    if (tmp_dir) {
        vfs_create_file(tmp_dir, "scratch.txt", "RamFS Scratch Space", 19);
        kprintf("ramfs: created /tmp (part of the one in-memory VFS)\n");
    }
}
