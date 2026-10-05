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

struct vnode;

// A device node has no stored contents: reading and writing it call these.
// Either may be NULL (reads then return end-of-file, writes fail).
// Both return bytes transferred or a negative error number, and may sleep.
typedef struct vnode_ops {
    // Files
    int64_t (*read)(struct vnode *node, size_t offset, void *buf, size_t count);
    int64_t (*write)(struct vnode *node, size_t offset, const void *buf, size_t count);
    int (*truncate)(struct vnode *node);   // Cut the file to zero length

    // Directories whose entries are stored somewhere (a disk). create makes
    // the entry and attaches the new node with vfs_attach; remove deletes
    // the entry (the VFS then frees the node). Both return 0 or a negative
    // error number.
    int (*create)(struct vnode *dir, const char *name, bool directory, struct vnode **out);
    int (*remove)(struct vnode *dir, struct vnode *child);
} vnode_ops_t;

typedef struct vnode {
    char name[128];             // Node name
    uint32_t type;              // VFS_FILE or VFS_DIRECTORY
    size_t size;                // File size in bytes
    const void *data;           // Pointer to file data
    bool owns_data;             // data is a kmalloc buffer this node must free
                                // (false: it points into the initrd or a constant)
    size_t capacity;            // Size of that buffer when owns_data
    uint32_t open_count;        // Open file descriptions referring to this node
    const vnode_ops_t *ops;     // Non-NULL for a node whose contents come from functions (/dev, /proc, files on disk)
    uint64_t private_data;      // For whoever provides ops (the FAT driver keeps the first cluster here)
    bool read_only;             // Nothing may be created, removed or opened for writing here
    uint32_t uid;               // Owner (see auth.h)
    uint16_t mode;              // Permission bits: who may read and write
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
/**
 * @brief Create a file or directory in `parent` (NULL: the root).
 * @param type VFS_FILE or VFS_DIRECTORY.
 * @return 0 and the new node in *out, or a negative error number
 *         (EEXIST, EROFS, EINVAL, ENOSPC, ...).
 */
int vfs_create(vnode_t *parent, const char *name, uint32_t type, vnode_t **out);

/**
 * @brief For filesystem drivers: add a node to the tree without asking any
 *        filesystem to store it and without permission checks.
 */
vnode_t *vfs_attach(vnode_t *parent, const char *name, uint32_t type);

vnode_t *vfs_create_dir(vnode_t *parent, const char *name);

/**
 * @brief Create a device node whose reads and writes are served by `ops`.
 */
vnode_t *vfs_create_device(vnode_t *parent, const char *name, const vnode_ops_t *ops);

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
/**
 * @brief Cut a file to `size` bytes (only shrinking is supported).
 */
void vfs_truncate(vnode_t *file, size_t size);

/**
 * @brief Find the directory that would contain `path`.
 * @param out_leaf Receives a pointer to the final component inside `path`.
 * @return The parent directory, or NULL if it does not exist or the path has
 *         no final component.
 */
vnode_t *vfs_find_parent(const char *path, const char **out_leaf);

/**
 * @brief As vfs_find_parent, with relative paths starting at `base`
 *        (NULL: the root).
 */
vnode_t *vfs_find_parent_at(vnode_t *base, const char *path, const char **out_leaf);

/**
 * @brief Write the absolute path of a node into buf.
 * @return Its length, or 0 if it does not fit.
 */
size_t vfs_path_of(vnode_t *node, char *buf, size_t size);

/**
 * @brief Find a direct child of a directory by name.
 */
vnode_t *vfs_find_child(vnode_t *dir, const char *name);

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
