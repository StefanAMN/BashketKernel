/**
 * @file vfs.c
 * @brief Implementation of Virtual Filesystem (VFS) Tree Management
 */

#include "vfs.h"
#include "mutex.h"
#include "heap.h"
#include "kprintf.h"
#include "string_util.h"
#include "kerrno.h"

static vnode_t *vfs_root = NULL;

void vfs_init(void) {
    if (vfs_root != NULL) return;

    vfs_root = (vnode_t *)kmalloc(sizeof(vnode_t));
    if (!vfs_root) panic("VFS: no memory for the root directory");
    memset(vfs_root, 0, sizeof(vnode_t));

    vfs_root->name[0] = '/';
    vfs_root->name[1] = '\0';
    vfs_root->type = VFS_DIRECTORY;
    vfs_root->mode = 0755;
    vfs_root->parent = NULL;
    vfs_root->children = NULL;
    vfs_root->next = NULL;

    kprintf("VFS Init: Root directory ('/') created successfully.\n");
}

// One lock for the whole tree: the links between nodes, their names, owners
// and modes, their open counts, and the contents of files kept in memory. It
// is a mutex because a filesystem behind a directory (FAT32) sleeps on the
// disk while creating or removing an entry. Every public function below
// takes it; a caller that does several steps which must not be separated
// (look a name up, then pin or change what it found) takes it around them
// with vfs_lock(), which nests.
static mutex_t vfs_mutex = MUTEX_INIT("vfs");

void vfs_lock(void) { mutex_lock(&vfs_mutex); }
void vfs_unlock(void) { mutex_unlock(&vfs_mutex); }

static vnode_t * vfs_find_child_unlocked(vnode_t *dir, const char *name);
static int vfs_find_parent_at_unlocked(vnode_t *base, const char *path, vnode_t **parent_out, char *leaf_out);
static size_t vfs_path_of_unlocked(vnode_t *node, char *buf, size_t size);
static vnode_t * vfs_attach_unlocked(vnode_t *parent, const char *name, uint32_t type);
static int vfs_create_unlocked(vnode_t *parent, const char *name, uint32_t type, vnode_t **out);
static vnode_t * vfs_create_dir_unlocked(vnode_t *parent, const char *name);
static vnode_t * vfs_create_file_unlocked(vnode_t *parent, const char *name, const void *data, size_t size);
static vnode_t * vfs_create_device_unlocked(vnode_t *parent, const char *name, const vnode_ops_t *ops);
static vnode_t * vfs_find_unlocked(vnode_t *relative_root, const char *path);
static void vfs_truncate_unlocked(vnode_t *file, size_t size);
static int vfs_unlink_unlocked(vnode_t *parent, const char *name);
static size_t vfs_read_unlocked(vnode_t *file, size_t offset, size_t count, void *buf);
static size_t vfs_write_unlocked(vnode_t *file, size_t offset, size_t count, const void *buf);

vnode_t *vfs_get_root(void) {
    return vfs_root;
}

// Whether `name` can be the name of a new entry: not empty, short enough
// for a node, no '/', and not "." or "..". Returns 0 or the error to give.
static int check_new_name(const char *name) {
    if (!name || name[0] == '\0' || strchr(name, '/')) return -EINVAL;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return -EINVAL;
    if (strlen(name) >= VFS_NAME_MAX) return -ENAMETOOLONG;
    return 0;
}

void vfs_load_dir(vnode_t *dir) {
    // A directory kept on a disk is read the first time anything looks
    // inside it. (The flag goes first: loading attaches nodes, which looks
    // inside the directory in turn.) Caller holds the tree lock.
    if (dir && dir->unloaded) {
        dir->unloaded = false;
        if (dir->ops && dir->ops->load) dir->ops->load(dir);
    }
}

// See vfs_find_child. Caller holds the tree lock.
static vnode_t * vfs_find_child_unlocked(vnode_t *dir, const char *name) {
    if (!dir || dir->type != VFS_DIRECTORY || !name) return NULL;
    vfs_load_dir(dir);
    for (vnode_t *child = dir->children; child; child = child->next) {
        if (strcmp(child->name, name) == 0) return child;
    }
    return NULL;
}

// See vfs_find_parent_at. Caller holds the tree lock.
static int vfs_find_parent_at_unlocked(vnode_t *base, const char *path, vnode_t **parent_out, char *leaf_out) {
    if (!path || !parent_out || !leaf_out) return -EINVAL;

    // The leaf is what follows the last '/' that has something after it
    const char *leaf = path;
    for (const char *p = path; *p != '\0'; p++) {
        if (*p == '/' && p[1] != '\0' && p[1] != '/') leaf = p + 1;
    }
    if (*leaf == '\0' || *leaf == '/') return -EINVAL;

    // "." and ".." name existing directories; nothing can be created or
    // removed under those names. Trailing slashes ("mkdir dir/") are not
    // part of the name.
    size_t leaf_len = 0;
    while (leaf[leaf_len] != '\0' && leaf[leaf_len] != '/') leaf_len++;
    if ((leaf_len == 1 && leaf[0] == '.') || (leaf_len == 2 && leaf[0] == '.' && leaf[1] == '.')) return -EINVAL;
    if (leaf_len >= VFS_NAME_MAX) return -ENAMETOOLONG;

    // Everything before the leaf names the parent
    size_t parent_len = (size_t)(leaf - path);
    char parent_path[VFS_PATH_MAX];
    if (parent_len >= sizeof(parent_path)) return -ENOENT;
    memcpy(parent_path, path, parent_len);
    parent_path[parent_len] = '\0';

    // No directory part means "in the base directory"
    vnode_t *parent = (parent_len == 0) ? (base ? base : vfs_root) : vfs_find(base, parent_path);
    if (!parent || parent->type != VFS_DIRECTORY) return -ENOENT;

    memcpy(leaf_out, leaf, leaf_len);
    leaf_out[leaf_len] = '\0';
    *parent_out = parent;
    return 0;
}

vnode_t *vfs_find_parent(const char *path, char leaf[VFS_NAME_MAX]) {
    vnode_t *parent = NULL;
    return vfs_find_parent_at(NULL, path, &parent, leaf) == 0 ? parent : NULL;
}

// See vfs_path_of. Caller holds the tree lock.
static size_t vfs_path_of_unlocked(vnode_t *node, char *buf, size_t size) {
    if (!node || !buf || size < 2) return 0;

    // Collect the names from the node up to the root, then write them out in
    // the opposite order
    const vnode_t *chain[32];
    size_t depth = 0;
    for (const vnode_t *n = node; n && n->parent; n = n->parent) {
        if (depth == 32) return 0;
        chain[depth++] = n;
    }

    size_t len = 0;
    if (depth == 0) buf[len++] = '/';
    while (depth > 0) {
        const char *name = chain[--depth]->name;
        size_t name_len = strlen(name);
        if (len + 1 + name_len + 1 > size) return 0;
        buf[len++] = '/';
        memcpy(buf + len, name, name_len);
        len += name_len;
    }
    buf[len] = '\0';
    return len;
}

// The child of `dir` called `name`, without reading the directory from its
// disk first (unlike vfs_find_child): what is already in memory. Caller
// holds the tree lock.
static vnode_t *child_in_memory(vnode_t *dir, const char *name) {
    for (vnode_t *c = dir->children; c; c = c->next) {
        if (strcmp(c->name, name) == 0) return c;
    }
    return NULL;
}

// Make a node called `name` of the given type and link it into `parent`'s
// children, owned by root with the default permissions. Nothing is written
// anywhere: filesystems call this for an entry they have read or just made.
// Caller holds the tree lock.
static vnode_t * vfs_attach_unlocked(vnode_t *parent, const char *name, uint32_t type) {
    if (!parent || parent->type != VFS_DIRECTORY || !name || name[0] == '\0') return NULL;
    // Cut short, the name would no longer be the one stored on the disk
    if (strlen(name) >= VFS_NAME_MAX) return NULL;

    vnode_t *node = (vnode_t *)kmalloc(sizeof(vnode_t));
    if (!node) return NULL;
    memset(node, 0, sizeof(vnode_t));
    memcpy(node->name, name, strlen(name) + 1);

    node->type = type;
    node->parent = parent;
    // Owned by root; readable by everyone, writable by the owner (and a
    // directory can be entered). Whoever creates the node on behalf of a
    // user changes owner and mode afterwards.
    node->uid = 0;
    node->gid = 0;
    node->mode = (type == VFS_DIRECTORY) ? 0755 : 0644;
    node->next = parent->children;
    parent->children = node;
    return node;
}

// See vfs_create. Caller holds the tree lock.
static int vfs_create_unlocked(vnode_t *parent, const char *name, uint32_t type, vnode_t **out) {
    if (!parent) parent = vfs_root;
    if (!name || parent->type != VFS_DIRECTORY) return -ENOTDIR;
    int bad = check_new_name(name);
    if (bad) return bad;
    if (vfs_find_child(parent, name)) return -EEXIST;
    if (parent->read_only) return -EROFS;

    vnode_t *node = NULL;
    if (parent->ops && parent->ops->create) {
        // The directory belongs to a filesystem that stores its entries
        // somewhere (a disk): it creates the entry there and attaches the node
        int err = parent->ops->create(parent, name, type == VFS_DIRECTORY, &node);
        if (err != 0) return err;
    } else {
        node = vfs_attach(parent, name, type);
    }
    if (!node) return -ENOMEM;

    if (out) *out = node;
    return 0;
}

// See vfs_create_dir. Caller holds the tree lock.
static vnode_t * vfs_create_dir_unlocked(vnode_t *parent, const char *name) {
    if (!parent) parent = vfs_root;

    // An existing directory of that name is returned as it is
    vnode_t *existing = vfs_find_child(parent, name);
    if (existing) return existing->type == VFS_DIRECTORY ? existing : NULL;

    vnode_t *dir = NULL;
    return vfs_create(parent, name, VFS_DIRECTORY, &dir) == 0 ? dir : NULL;
}

// See vfs_create_file. Caller holds the tree lock.
static vnode_t * vfs_create_file_unlocked(vnode_t *parent, const char *name, const void *data, size_t size) {
    if (!parent) parent = vfs_root;

    // Initial contents by pointer only make sense for a file kept in memory
    if (data && parent->ops && parent->ops->create) return NULL;

    vnode_t *file = NULL;
    if (vfs_create(parent, name, VFS_FILE, &file) != 0) return NULL;
    if (data) {
        file->size = size;
        file->data = data;
    }
    return file;
}

// See vfs_create_device. Caller holds the tree lock.
static vnode_t * vfs_create_device_unlocked(vnode_t *parent, const char *name, const vnode_ops_t *ops) {
    if (!ops) return NULL;
    vnode_t *node = vfs_create_file(parent, name, NULL, 0);
    if (node) node->ops = ops;
    return node;
}

// See vfs_mount_dir. Caller holds the tree lock.
static int vfs_mount_dir_unlocked(vnode_t *parent, const char *name, vnode_t **out) {
    if (!parent) parent = vfs_root;
    if (parent->type != VFS_DIRECTORY || !name) return -ENOTDIR;
    if (check_new_name(name)) return -EINVAL;

    // The parent's entries are read first, so that a directory of this name
    // on the disk is found now and hidden, rather than turning up later
    vfs_load_dir(parent);
    vnode_t *hidden = child_in_memory(parent, name);
    if (hidden) {
        if (hidden->type != VFS_DIRECTORY) return -ENOTDIR;
        if (hidden->mount_point || hidden->open_count > 0) return -EBUSY;
        if (!hidden->unloaded && hidden->children) return -EBUSY; // Something below it is known, perhaps in use
    }

    // The new node goes in first; only then is the hidden one taken out, so
    // a failure leaves the tree as it was
    vnode_t *node = (vnode_t *)kmalloc(sizeof(vnode_t));
    if (!node) return -ENOMEM;
    memset(node, 0, sizeof(vnode_t));
    strncpy(node->name, name, sizeof(node->name) - 1);
    node->type = VFS_DIRECTORY;
    node->parent = parent;
    node->mode = hidden ? hidden->mode : 0755;
    node->uid = hidden ? hidden->uid : 0;
    node->gid = hidden ? hidden->gid : 0;
    node->mount_point = true;
    node->covered = hidden;

    if (hidden) {
        vnode_t **link = &parent->children;
        while (*link && *link != hidden) link = &(*link)->next;
        if (*link) *link = hidden->next;
        hidden->next = NULL;
    }
    node->next = parent->children;
    parent->children = node;
    if (out) *out = node;
    return 0;
}

// See vfs_unmount_dir. Caller holds the tree lock.
static int vfs_unmount_dir_unlocked(vnode_t *node) {
    if (!node || !node->mount_point) return -EINVAL;
    if (node->open_count > 0 || node->children || node->ops) return -EBUSY;

    vnode_t *parent = node->parent;
    vnode_t **link = &parent->children;
    while (*link && *link != node) link = &(*link)->next;
    if (*link) *link = node->next;

    // The directory it hid comes back as it was
    if (node->covered) {
        node->covered->next = parent->children;
        parent->children = node->covered;
    }
    kfree(node);
    return 0;
}

// See vfs_find. Caller holds the tree lock.
static vnode_t * vfs_find_unlocked(vnode_t *relative_root, const char *path) {
    if (!path) return NULL;

    // An absolute path starts at the root whatever the caller's directory is
    vnode_t *node = (relative_root && path[0] != '/') ? relative_root : vfs_root;

    while (*path != '\0') {
        while (*path == '/') path++;          // Skip separators, including repeated ones
        if (*path == '\0') break;

        char token[128];
        size_t len = 0;
        while (path[len] != '\0' && path[len] != '/') len++;
        if (len >= sizeof(token)) return NULL; // No name is that long
        memcpy(token, path, len);
        token[len] = '\0';
        path += len;

        // Only a directory can have something looked up inside it
        if (node->type != VFS_DIRECTORY) return NULL;

        if (strcmp(token, ".") == 0) {
            continue;                          // The directory itself
        } else if (strcmp(token, "..") == 0) {
            if (node->parent) node = node->parent; // The root is its own parent
        } else {
            node = vfs_find_child(node, token);
            if (!node) return NULL;
        }
    }

    return node;
}

// See vfs_read. Caller holds the tree lock unless the node has `ops`.
static size_t vfs_read_unlocked(vnode_t *file, size_t offset, size_t count, void *buf) {
    if (!file || file->type != VFS_FILE || !buf) return 0;
    if (file->ops) {
        int64_t n = file->ops->read ? file->ops->read(file, offset, buf, count) : 0;
        return n > 0 ? (size_t)n : 0;
    }
    if (!file->data || offset >= file->size) return 0;

    size_t available = file->size - offset;
    size_t to_read = (count < available) ? count : available;

    const uint8_t *src = (const uint8_t *)file->data + offset;
    memcpy(buf, src, to_read);
    return to_read;
}

// See vfs_write. Caller holds the tree lock unless the node has `ops`.
static size_t vfs_write_unlocked(vnode_t *file, size_t offset, size_t count, const void *buf) {
    if (!file || file->type != VFS_FILE || !buf || count == 0) return 0;
    if (file->ops) {
        int64_t n = file->ops->write ? file->ops->write(file, offset, buf, count) : 0;
        return n > 0 ? (size_t)n : 0;
    }

    size_t end = offset + count;
    if (end < offset) return 0; // Overflow

    // Files from the initrd or from string constants are not ours to modify.
    // The first write moves the contents into a buffer the node owns; later
    // writes reuse it, doubling its capacity when it fills up.
    if (!file->owns_data || end > file->capacity) {
        size_t capacity = file->owns_data ? file->capacity : 0;
        if (capacity < 64) capacity = 64;
        while (capacity < end || capacity < file->size) capacity *= 2;

        uint8_t *grown = (uint8_t *)kmalloc(capacity);
        if (!grown) return 0;
        if (file->data && file->size > 0) memcpy(grown, file->data, file->size);

        if (file->owns_data) kfree((void *)file->data);
        file->data = grown;
        file->capacity = capacity;
        file->owns_data = true;
    }

    uint8_t *data = (uint8_t *)file->data;
    if (offset > file->size) {
        memset(data + file->size, 0, offset - file->size); // A gap reads as zeros
    }
    memcpy(data + offset, buf, count);
    if (end > file->size) file->size = end;

    return count;
}

// See vfs_truncate. Caller holds the tree lock.
static void vfs_truncate_unlocked(vnode_t *file, size_t size) {
    if (!file || file->type != VFS_FILE) return;

    if (file->ops) {
        // Only cutting to nothing is offered to filesystems
        if (size == 0 && file->ops->truncate) file->ops->truncate(file);
        return;
    }
    if (size < file->size) file->size = size;
}

// See vfs_unlink. Caller holds the tree lock.
static int vfs_unlink_unlocked(vnode_t *parent, const char *name) {
    if (!parent) parent = vfs_root;
    if (!name || parent->type != VFS_DIRECTORY) return -ENOTDIR;
    if (parent->read_only) return -EROFS;

    vfs_load_dir(parent);
    for (vnode_t *curr = parent->children; curr; curr = curr->next) {
        if (strcmp(curr->name, name) != 0) continue;

        if (curr->mount_point) return -EBUSY;  // Unmount it first (vfs_unmount_dir)
        vfs_load_dir(curr); // Whether it is empty is only known once it has been read
        if (curr->children) return -ENOTEMPTY;
        if (curr->open_count > 0) return -EBUSY; // Someone holds it open

        // Let the owning filesystem remove it from its storage first; if that
        // fails the node stays
        if (parent->ops && parent->ops->remove) {
            int err = parent->ops->remove(parent, curr);
            if (err != 0) return err;
        }

        // The filesystem's call may have slept (the tree lock is a mutex,
        // so nothing changed meanwhile, but the walk is cheap): unlink it
        vnode_t **link = &parent->children;
        while (*link && *link != curr) link = &(*link)->next;
        if (*link) *link = curr->next;

        if (curr->owns_data) kfree((void *)curr->data);
        kfree(curr);
        return 0;
    }

    return -ENOENT;
}

// See vfs_rename. Caller holds the tree lock.
static int vfs_rename_unlocked(vnode_t *old_parent, const char *old_name, vnode_t *new_parent, const char *new_name) {
    if (!old_parent) old_parent = vfs_root;
    if (!new_parent) new_parent = vfs_root;
    if (!old_name) return -EINVAL;
    if (old_parent->type != VFS_DIRECTORY || new_parent->type != VFS_DIRECTORY) return -ENOTDIR;
    int bad = check_new_name(new_name);
    if (bad) return bad;
    if (old_parent->read_only || new_parent->read_only) return -EROFS;

    vnode_t *node = vfs_find_child_unlocked(old_parent, old_name);
    if (!node) return -ENOENT;
    if (node->mount_point) return -EBUSY;
    vnode_t *existing = vfs_find_child_unlocked(new_parent, new_name);
    if (existing == node) return 0;                     // Onto itself: nothing to do
    if (existing) return -EEXIST;                       // Nothing is replaced: remove it first

    // A directory can not be moved into itself or anything below itself
    for (vnode_t *up = new_parent; up; up = up->parent) {
        if (up == node) return -EINVAL;
    }

    // Both places must belong to the same filesystem: a move between two
    // is a copy and a delete, which is a program's job, not the kernel's
    if (old_parent->ops != new_parent->ops) return -EXDEV;
    if (old_parent->ops) {
        if (!old_parent->ops->rename) return -EROFS;
        int err = old_parent->ops->rename(old_parent, node, new_parent, new_name);
        if (err != 0) return err;
    }

    // The node itself: out of one list of children, into the other
    vnode_t **link = &old_parent->children;
    while (*link && *link != node) link = &(*link)->next;
    if (*link) *link = node->next;
    memset(node->name, 0, sizeof(node->name));
    strncpy(node->name, new_name, sizeof(node->name) - 1);
    node->parent = new_parent;
    node->next = new_parent->children;
    new_parent->children = node;
    return 0;
}

int vfs_rename(vnode_t *old_parent, const char *old_name, vnode_t *new_parent, const char *new_name) {
    vfs_lock();
    int result = vfs_rename_unlocked(old_parent, old_name, new_parent, new_name);
    vfs_unlock();
    return result;
}

#ifdef KERNEL_TESTS
bool vfs_forget_directory(vnode_t *dir) {
    if (!dir || !dir->ops || !dir->ops->load || dir->unloaded) return false;
    vfs_lock();
    for (vnode_t *c = dir->children; c; c = c->next) {
        if (!c->mount_point && (c->open_count != 0 || c->children)) {
            vfs_unlock();
            return false;
        }
    }
    // Mount points stay: they are not the disk's (vfs_mount_dir)
    vnode_t **link = &dir->children;
    while (*link) {
        vnode_t *c = *link;
        if (c->mount_point) {
            link = &c->next;
            continue;
        }
        *link = c->next;
        kfree((void *)c->private_data); // The filesystem's record of the entry (a plain kmalloc block)
        kfree(c);
    }
    dir->unloaded = true;
    vfs_unlock();
    return true;
}
#endif

// --- The public functions: each of the above, under the tree lock ----------

vnode_t *vfs_find_child(vnode_t *dir, const char *name) {
    vfs_lock();
    vnode_t *result = vfs_find_child_unlocked(dir, name);
    vfs_unlock();
    return result;
}

int vfs_find_parent_at(vnode_t *base, const char *path, vnode_t **parent, char leaf[VFS_NAME_MAX]) {
    vfs_lock();
    int result = vfs_find_parent_at_unlocked(base, path, parent, leaf);
    vfs_unlock();
    return result;
}

size_t vfs_path_of(vnode_t *node, char *buf, size_t size) {
    vfs_lock();
    size_t result = vfs_path_of_unlocked(node, buf, size);
    vfs_unlock();
    return result;
}

vnode_t *vfs_attach(vnode_t *parent, const char *name, uint32_t type) {
    vfs_lock();
    vnode_t *result = vfs_attach_unlocked(parent, name, type);
    vfs_unlock();
    return result;
}

int vfs_create(vnode_t *parent, const char *name, uint32_t type, vnode_t **out) {
    vfs_lock();
    int result = vfs_create_unlocked(parent, name, type, out);
    vfs_unlock();
    return result;
}

vnode_t *vfs_create_dir(vnode_t *parent, const char *name) {
    vfs_lock();
    vnode_t *result = vfs_create_dir_unlocked(parent, name);
    vfs_unlock();
    return result;
}

int vfs_mount_dir(vnode_t *parent, const char *name, vnode_t **out) {
    vfs_lock();
    int result = vfs_mount_dir_unlocked(parent, name, out);
    vfs_unlock();
    return result;
}

int vfs_unmount_dir(vnode_t *node) {
    vfs_lock();
    int result = vfs_unmount_dir_unlocked(node);
    vfs_unlock();
    return result;
}

vnode_t *vfs_create_file(vnode_t *parent, const char *name, const void *data, size_t size) {
    vfs_lock();
    vnode_t *result = vfs_create_file_unlocked(parent, name, data, size);
    vfs_unlock();
    return result;
}

vnode_t *vfs_create_device(vnode_t *parent, const char *name, const vnode_ops_t *ops) {
    vfs_lock();
    vnode_t *result = vfs_create_device_unlocked(parent, name, ops);
    vfs_unlock();
    return result;
}

vnode_t *vfs_find(vnode_t *relative_root, const char *path) {
    vfs_lock();
    vnode_t *result = vfs_find_unlocked(relative_root, path);
    vfs_unlock();
    return result;
}

void vfs_truncate(vnode_t *file, size_t size) {
    vfs_lock();
    vfs_truncate_unlocked(file, size);
    vfs_unlock();
}

int vfs_unlink(vnode_t *parent, const char *name) {
    vfs_lock();
    int result = vfs_unlink_unlocked(parent, name);
    vfs_unlock();
    return result;
}

size_t vfs_read(vnode_t *file, size_t offset, size_t count, void *buf) {
    // A node with its own operations (a device, /proc, a disk file) looks
    // after itself, and may wait a long time (a terminal read): the tree
    // lock is not held for it. An open node can not go away meanwhile.
    if (file && file->ops) return vfs_read_unlocked(file, offset, count, buf);
    vfs_lock();
    size_t result = vfs_read_unlocked(file, offset, count, buf);
    vfs_unlock();
    return result;
}

size_t vfs_write(vnode_t *file, size_t offset, size_t count, const void *buf) {
    // As in vfs_read: a node with its own operations is not written under
    // the tree lock
    if (file && file->ops) return vfs_write_unlocked(file, offset, count, buf);
    vfs_lock();
    size_t result = vfs_write_unlocked(file, offset, count, buf);
    vfs_unlock();
    return result;
}
