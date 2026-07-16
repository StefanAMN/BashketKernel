/**
 * @file mount.h
 * @brief VFS Mount Point Registry Interface
 */

#ifndef MOUNT_H
#define MOUNT_H

#include <stdint.h>
#include "vfs.h"

/**
 * @brief Register a mount point in the VFS mount table.
 * @param path Mount point path.
 * @param root_vnode Target filesystem root vnode.
 * @return 0 on success, -1 on failure.
 */
int vfs_mount(const char *path, vnode_t *root_vnode);

/**
 * @brief Unmount a registered VFS path.
 * @param path Mount point path to unmount.
 * @return 0 on success, -1 on failure.
 */
int vfs_umount(const char *path);

/**
 * @brief Print active VFS mount table to console.
 */
void vfs_list_mounts(void);

#endif // MOUNT_H
