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

#define VFS_NAME_MAX  128  // Size of a node's name, with its NUL: names are at most 127 bytes
#define VFS_PATH_MAX  256  // Size of a path the kernel accepts, with its NUL

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

    // Give `node` (a child of old_dir) the name new_name in new_dir, which
    // belongs to the same filesystem. The VFS then moves the node itself.
    int (*rename)(struct vnode *old_dir, struct vnode *node, struct vnode *new_dir, const char *new_name);

    // Read the directory's entries from wherever they are stored and attach
    // a node for each. Called once, the first time something looks inside a
    // directory marked `unloaded`.
    int (*load)(struct vnode *dir);

    // Record the node's owner, group and permission bits (as the vnode has
    // them now) where the filesystem keeps them. A filesystem without this
    // cannot store them: what it shows for them is fixed.
    int (*setattr)(struct vnode *node);

    // Told when the node is opened and when that open file is closed for
    // good (by its last holder, or because its last holder died), for
    // devices that change state while a program has them. `opened` may
    // refuse by returning a negative error number.
    int (*opened)(struct vnode *node, bool writable);
    void (*closed)(struct vnode *node, bool writable);

    // For poll(): which of POLLIN, POLLOUT, POLLERR, POLLHUP (poll.h) are
    // true of the device now. NULL: always readable and writable. Whoever
    // provides it calls poll_notify() whenever the answer may change.
    int (*poll)(struct vnode *node);
} vnode_ops_t;

typedef struct vnode {
    char name[VFS_NAME_MAX];    // Node name
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
    bool unloaded;              // A directory whose children have not been read yet (see ops->load)
    uint32_t mtime;             // When it was last written: seconds since 1970 (UTC); 0: not known
    uint32_t uid;               // Owner (see auth.h)
    uint32_t gid;               // Group
    uint16_t mode;              // Permission bits: who may read, write and execute; set-uid
    struct vnode *parent;       // Pointer to parent directory node
    struct vnode *children;     // Pointer to first child node if directory
    struct vnode *next;         // Pointer to next sibling in directory listing
    bool mount_point;           // Put here by vfs_mount_dir: may not be removed or renamed
    struct vnode *covered;      // For a mount point: the directory of the same name it hides, if there was one
} vnode_t;

/**
 * @brief Create the root directory ('/'), empty and kept in memory. Called
 *        by the first thing that builds the tree; later calls do nothing.
 *        Panics if there is no memory for it.
 */
void vfs_init(void);

/**
 * @brief The root directory node ('/'). NULL before vfs_init.
 */
vnode_t *vfs_get_root(void);

/**
 * @brief Take the lock on the file tree, for a sequence of steps that must
 *        not be separated: looking a name up and then using what was found.
 *        A node pointer is only safe to use while this is held, or while the
 *        node is open (its open_count is not zero). May sleep; nests.
 *        Every vfs_ function below takes it itself.
 */
void vfs_lock(void);

/**
 * @brief Release one hold of the tree lock (vfs_lock).
 */
void vfs_unlock(void);

/**
 * @brief Find the node a path names: absolute ("/bin/hello"), or relative
 *        to `relative_root` ("hello", NULL: the root). "." and ".." are
 *        understood, repeated slashes ignored; directories on a disk are
 *        read as the walk enters them (which may sleep).
 * @return The node, or NULL if any component is missing, is not a
 *         directory where one is needed, or is longer than a name can be.
 *         The pointer is only safe under vfs_lock (see above).
 */
vnode_t *vfs_find(vnode_t *relative_root, const char *path);

/**
 * @brief Create a file in `parent` (NULL: the root), with `data` as its
 *        contents if given. Contents given this way are not copied (the
 *        node points at them until its first write), so they must stay;
 *        that is only allowed in a directory kept in memory.
 * @return The new node, or NULL if it could not be made (see vfs_create).
 */
vnode_t *vfs_create_file(vnode_t *parent, const char *name, const void *data, size_t size);

/**
 * @brief Create a file or directory in `parent` (NULL: the root): on its
 *        disk if the directory has a `create` operation, else in memory.
 *        Owned by root, mode 0644 or 0755; whoever creates it for a user
 *        changes that afterwards. No permission checks.
 * @param type VFS_FILE or VFS_DIRECTORY.
 * @return 0 and the new node in *out (if `out` is not NULL), or a negative
 *         error number: EEXIST, EROFS, ENOTDIR (parent is not a directory),
 *         EINVAL (an empty name, one with '/', "." or ".."), ENAMETOOLONG
 *         (VFS_NAME_MAX bytes or more), ENOSPC or EIO from the disk, ENOMEM.
 */
int vfs_create(vnode_t *parent, const char *name, uint32_t type, vnode_t **out);

/**
 * @brief For filesystem drivers: add a node to the tree without asking any
 *        filesystem to store it and without permission checks (for an entry
 *        a driver has read from its disk, or has just written there). Owned
 *        by root, mode 0644 or 0755.
 * @return The node, or NULL for a name that is empty or too long for a
 *         node (VFS_NAME_MAX), or when memory ran out.
 */
vnode_t *vfs_attach(vnode_t *parent, const char *name, uint32_t type);

/**
 * @brief The directory `name` in `parent` (NULL: the root): the existing one
 *        if there is one, else a new one made as vfs_create would.
 * @return NULL if it could not be made, or the name is taken by a file.
 */
vnode_t *vfs_create_dir(vnode_t *parent, const char *name);

/**
 * @brief Put an empty directory kept in memory at `parent`/`name`, as a
 *        place to attach another tree: /dev, /proc and /tmp on a root that
 *        lives on a disk, or a disk volume's root at /disk.
 *
 * Nothing is written to the parent's filesystem. If the parent already has a
 * directory of that name (on a disk, the empty directory left there for the
 * purpose), it is hidden, not changed: it disappears from the listing, its
 * node is kept aside, and vfs_unmount_dir puts it back. When the parent is
 * read from the disk again, the disk's entry stays hidden: a filesystem's
 * load skips names its directory already has, and a mount point is never
 * dropped by forgetting the parent's contents.
 *
 * The result can be neither removed nor renamed (-EBUSY) until it is
 * unmounted. Whoever mounted it may give it `ops` (a filesystem's root).
 *
 * @param out Receives the new node.
 * @return 0; -ENOTDIR if `parent` is not a directory or the name is taken
 *         by a file; -EINVAL for a name a new entry cannot have; -EBUSY if it
 *         is taken by a directory that is open, has contents already read,
 *         or is a mount point itself; -ENOMEM.
 */
int vfs_mount_dir(vnode_t *parent, const char *name, vnode_t **out);

/**
 * @brief Take away a mount point made by vfs_mount_dir, and show the
 *        directory it hid again.
 * @return 0; -EINVAL if `node` is not a mount point; -EBUSY if it is open,
 *         has anything in it, or has a filesystem attached (`ops`).
 */
int vfs_unmount_dir(vnode_t *node);

/**
 * @brief Create a device node in `parent` whose reads and writes are served
 *        by `ops` (which must stay valid: pass a static table). If a node of
 *        that name exists, nothing is changed.
 * @return The new node, or NULL if it existed already or could not be made.
 */
vnode_t *vfs_create_device(vnode_t *parent, const char *name, const vnode_ops_t *ops);

/**
 * @brief Read up to `count` bytes of a file from `offset` into the kernel
 *        buffer `buf`. A node with `ops` is read through them, outside the
 *        tree lock (it may sleep: a terminal, a disk); the caller must have
 *        it open so it cannot be removed meanwhile.
 * @return Bytes read; 0 at or past the end, for a directory, and also when
 *         the node's read operation failed (callers that must tell an error
 *         from the end call ops->read themselves, as file_read does).
 */
size_t vfs_read(vnode_t *file, size_t offset, size_t count, void *buf);

/**
 * @brief Write `count` bytes from `buf` at `offset`. A file kept in memory
 *        grows as needed (a gap reads as zeros); the first write copies
 *        contents that pointed into the initrd into a buffer of its own.
 *        A node with `ops` is written through them, as in vfs_read.
 * @return Bytes written; 0 if nothing could be (memory ran out, a node with
 *         no write operation, a failed write).
 */
size_t vfs_write(vnode_t *file, size_t offset, size_t count, const void *buf);

/**
 * @brief Cut a file to `size` bytes. Only shrinking is supported; a file on
 *        a disk can only be cut to nothing (its `truncate` operation).
 */
void vfs_truncate(vnode_t *file, size_t size);

/**
 * @brief Find the directory that would contain `path`: absolute, or
 *        relative to `base` (NULL: the root).
 * @param parent Receives the directory (only on success).
 * @param leaf Receives the final component, without trailing slashes:
 *        "/tmp/dir/" gives /tmp and "dir". At least VFS_NAME_MAX bytes.
 * @return 0; -ENOENT if the directory does not exist (or is a file);
 *         -EINVAL if the path has no final component or it is "." or "..";
 *         -ENAMETOOLONG if the final component is too long for a name.
 */
int vfs_find_parent_at(vnode_t *base, const char *path, vnode_t **parent, char leaf[VFS_NAME_MAX]);

/**
 * @brief vfs_find_parent_at from the root, returning just the directory
 *        (NULL for any of its errors).
 */
vnode_t *vfs_find_parent(const char *path, char leaf[VFS_NAME_MAX]);

/**
 * @brief Write the absolute path of a node into buf.
 * @return Its length, or 0 if it does not fit (or is more than 32 levels
 *         deep).
 */
size_t vfs_path_of(vnode_t *node, char *buf, size_t size);

/**
 * @brief Find a direct child of a directory by name, reading the directory
 *        from its disk first if it has not been (may sleep).
 * @return The child, or NULL (also when `dir` is not a directory).
 */
vnode_t *vfs_find_child(vnode_t *dir, const char *name);

/**
 * @brief Remove the file or empty directory `name` from `parent` (NULL: the
 *        root): from its disk first, then from the tree, freeing the node.
 *        No permission checks.
 * @return 0; -ENOENT; -ENOTEMPTY; -EBUSY if it is open, someone's working
 *         directory, mapped, or a mount point; -EROFS; or the disk's error.
 */
int vfs_unlink(vnode_t *parent, const char *name);

/**
 * @brief Give an entry a new name, in the same directory or another of the
 *        same filesystem. Nothing is replaced: -EEXIST if the new name is
 *        taken. -EXDEV across filesystems; -EINVAL to move a directory into
 *        itself or for a name a new entry cannot have; -EBUSY for a mount
 *        point.
 */
int vfs_rename(vnode_t *old_parent, const char *old_name, vnode_t *new_parent, const char *new_name);

#ifdef KERNEL_TESTS
/**
 * @brief For tests: drop what is known of a disk directory's contents (not
 *        its mount points), so that the next look inside reads them from the
 *        disk again. To test what was written, forget it first. False,
 *        changing nothing, if anything in it is open or has been read into,
 *        or `dir` is not a directory of a filesystem that loads from a disk.
 */
bool vfs_forget_directory(vnode_t *dir);
#endif

/**
 * @brief Make sure a directory's children have been read from its storage
 *        (see vnode_ops_t.load). Call, holding the tree lock, before
 *        walking dir->children directly.
 */
void vfs_load_dir(vnode_t *dir);

#endif // VFS_H
