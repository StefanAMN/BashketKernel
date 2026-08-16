/**
 * @file vfs.h
 * @brief Virtual Filesystem (VFS) Abstraction Interface
 */

#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define VFS_FILE      1
#define VFS_DIRECTORY 2

typedef struct vnode {
    char name[128];             // Node name
    uint32_t type;              // VFS_FILE or VFS_DIRECTORY
    size_t size;                // File size in bytes
    const void *data;           // Pointer to file data
    struct vnode *parent;       // Pointer to parent directory node
    struct vnode *children;     // Pointer to first child node if directory
    struct vnode *next;         // Pointer to next sibling in directory listing
} vnode_t;

/**
 * @brief Initialize Virtual Filesystem root directory ('/').
 */
void vfs_init(void);

/**
 * @brief Get VFS root directory node ('/').
 * @return Pointer to root vnode.
 */
vnode_t *vfs_get_root(void);

/**
 * @brief Find file or directory node by path (e.g. "/bin/hello" or "hello").
 * @param relative_root Directory node to search from (or NULL for VFS root).
 * @param path File path string.
 * @return Pointer to matching vnode, or NULL if not found.
 */
vnode_t *vfs_find(vnode_t *relative_root, const char *path);

/**
 * @brief Create a new file vnode under specified parent directory.
 */
vnode_t *vfs_create_file(vnode_t *parent, const char *name, const void *data, size_t size);

/**
 * @brief Create a new directory vnode under specified parent directory.
 */
vnode_t *vfs_create_dir(vnode_t *parent, const char *name);

/**
 * @brief Read bytes from a file vnode.
 * @param file Pointer to file vnode.
 * @param offset Byte offset to start reading from.
 * @param count Maximum number of bytes to read.
 * @param buf Output buffer.
 * @return Number of bytes successfully read.
 */
size_t vfs_read(vnode_t *file, size_t offset, size_t count, void *buf);

/**
 * @brief Write bytes to a dynamic heap-allocated file vnode.
 * @param file Pointer to file vnode.
 * @param offset Byte offset to start writing at.
 * @param count Number of bytes to write.
 * @param buf Data input buffer.
 * @return Number of bytes successfully written.
 */
size_t vfs_write(vnode_t *file, size_t offset, size_t count, const void *buf);

/**
 * @brief Delete/unlink file or empty directory node from VFS.
 * @param parent Directory vnode containing the file.
 * @param name File or directory name to delete.
 * @return 0 on success, -1 on failure.
 */
int vfs_unlink(vnode_t *parent, const char *name);

/**
 * @brief Query metadata statistics for a VFS vnode.
 * @param node VFS node to query.
 * @param out_size Pointer to store file size.
 * @param out_type Pointer to store node type.
 * @return 0 on success, -1 on failure.
 */
int vfs_stat(vnode_t *node, size_t *out_size, uint32_t *out_type);

#endif // VFS_H
