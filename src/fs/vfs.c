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
static vnode_t * vfs_find_parent_at_unlocked(vnode_t *base, const char *path, const char **out_leaf);
static size_t vfs_path_of_unlocked(vnode_t *node, char *buf, size_t size);
static vnode_t * vfs_attach_unlocked(vnode_t *parent, const char *name, uint32_t type);
static int vfs_create_unlocked(vnode_t *parent, const char *name, uint32_t type, vnode_t **out);
static vnode_t * vfs_create_dir_unlocked(vnode_t *parent, const char *name);
static vnode_t * vfs_create_file_unlocked(vnode_t *parent, const char *name, const void *data, size_t size);
static vnode_t * vfs_create_device_unlocked(vnode_t *parent, const char *name, const vnode_ops_t *ops);
static vnode_t * vfs_find_unlocked(vnode_t *relative_root, const char *path);
static void vfs_truncate_unlocked(vnode_t *file, size_t size);
static int vfs_unlink_unlocked(vnode_t *parent, const char *name);
static int vfs_stat_unlocked(vnode_t *node, size_t *out_size, uint32_t *out_type);
static size_t vfs_read_unlocked(vnode_t *file, size_t offset, size_t count, void *buf);
static size_t vfs_write_unlocked(vnode_t *file, size_t offset, size_t count, const void *buf);

vnode_t *vfs_get_root(void) {
    return vfs_root;
}

static vnode_t * vfs_find_child_unlocked(vnode_t *dir, const char *name) {
    if (!dir || dir->type != VFS_DIRECTORY || !name) return NULL;
    for (vnode_t *child = dir->children; child; child = child->next) {
        if (strcmp(child->name, name) == 0) return child;
    }
    return NULL;
}

static vnode_t * vfs_find_parent_at_unlocked(vnode_t *base, const char *path, const char **out_leaf) {
    if (!path || !out_leaf) return NULL;

    // The leaf is what follows the last '/' that has something after it
    const char *leaf = path;
    for (const char *p = path; *p != '\0'; p++) {
        if (*p == '/' && p[1] != '\0' && p[1] != '/') leaf = p + 1;
    }
    if (*leaf == '\0' || *leaf == '/') return NULL;

    // "." and ".." name existing directories; nothing can be created or
    // removed under those names
    size_t leaf_len = 0;
    while (leaf[leaf_len] != '\0' && leaf[leaf_len] != '/') leaf_len++;
    if ((leaf_len == 1 && leaf[0] == '.') || (leaf_len == 2 && leaf[0] == '.' && leaf[1] == '.')) return NULL;

    // Everything before the leaf names the parent
    size_t parent_len = (size_t)(leaf - path);
    char parent_path[128];
    if (parent_len >= sizeof(parent_path)) return NULL;
    memcpy(parent_path, path, parent_len);
    parent_path[parent_len] = '\0';

    // No directory part means "in the base directory"
    vnode_t *parent = (parent_len == 0) ? (base ? base : vfs_root) : vfs_find(base, parent_path);
    if (!parent || parent->type != VFS_DIRECTORY) return NULL;

    *out_leaf = leaf;
    return parent;
}

vnode_t *vfs_find_parent(const char *path, const char **out_leaf) {
    return vfs_find_parent_at(NULL, path, out_leaf);
}

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

static vnode_t * vfs_attach_unlocked(vnode_t *parent, const char *name, uint32_t type) {
    if (!parent || parent->type != VFS_DIRECTORY || !name || name[0] == '\0') return NULL;

    vnode_t *node = (vnode_t *)kmalloc(sizeof(vnode_t));
    if (!node) return NULL;
    memset(node, 0, sizeof(vnode_t));

    size_t i = 0;
    for (; name[i] != '\0' && i < sizeof(node->name) - 1; i++) {
        node->name[i] = name[i];
    }
    node->name[i] = '\0';

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

static int vfs_create_unlocked(vnode_t *parent, const char *name, uint32_t type, vnode_t **out) {
    if (!parent) parent = vfs_root;
    if (!name || parent->type != VFS_DIRECTORY) return -ENOTDIR;
    if (name[0] == '\0') return -EINVAL;
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

static vnode_t * vfs_create_dir_unlocked(vnode_t *parent, const char *name) {
    if (!parent) parent = vfs_root;

    // An existing directory of that name is returned as it is
    vnode_t *existing = vfs_find_child(parent, name);
    if (existing) return existing->type == VFS_DIRECTORY ? existing : NULL;

    vnode_t *dir = NULL;
    return vfs_create(parent, name, VFS_DIRECTORY, &dir) == 0 ? dir : NULL;
}

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

static vnode_t * vfs_create_device_unlocked(vnode_t *parent, const char *name, const vnode_ops_t *ops) {
    if (!ops) return NULL;
    vnode_t *node = vfs_create_file(parent, name, NULL, 0);
    if (node) node->ops = ops;
    return node;
}

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

static void vfs_truncate_unlocked(vnode_t *file, size_t size) {
    if (!file || file->type != VFS_FILE) return;

    if (file->ops) {
        // Only cutting to nothing is offered to filesystems
        if (size == 0 && file->ops->truncate) file->ops->truncate(file);
        return;
    }
    if (size < file->size) file->size = size;
}

static int vfs_unlink_unlocked(vnode_t *parent, const char *name) {
    if (!parent) parent = vfs_root;
    if (!name || parent->type != VFS_DIRECTORY) return -ENOTDIR;
    if (parent->read_only) return -EROFS;

    vnode_t *prev = NULL;
    for (vnode_t *curr = parent->children; curr; prev = curr, curr = curr->next) {
        if (strcmp(curr->name, name) != 0) continue;

        if (curr->children) return -ENOTEMPTY;
        if (curr->open_count > 0) return -EBUSY; // Someone holds it open

        // Let the owning filesystem remove it from its storage first; if that
        // fails the node stays
        if (parent->ops && parent->ops->remove) {
            int err = parent->ops->remove(parent, curr);
            if (err != 0) return err;
        }

        // The filesystem's call may have slept: find the node's place in the
        // list again rather than trusting `prev`
        vnode_t **link = &parent->children;
        while (*link && *link != curr) link = &(*link)->next;
        if (*link) *link = curr->next;
        (void)prev;

        if (curr->owns_data) kfree((void *)curr->data);
        kfree(curr);
        return 0;
    }

    return -ENOENT;
}

static int vfs_stat_unlocked(vnode_t *node, size_t *out_size, uint32_t *out_type) {
    if (!node) return -1;
    if (out_size) *out_size = node->size;
    if (out_type) *out_type = node->type;
    return 0;
}

// --- The public functions: each of the above, under the tree lock ----------

vnode_t *vfs_find_child(vnode_t *dir, const char *name) {
    vfs_lock();
    vnode_t *result = vfs_find_child_unlocked(dir, name);
    vfs_unlock();
    return result;
}

vnode_t *vfs_find_parent_at(vnode_t *base, const char *path, const char **out_leaf) {
    vfs_lock();
    vnode_t *result = vfs_find_parent_at_unlocked(base, path, out_leaf);
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

int vfs_stat(vnode_t *node, size_t *out_size, uint32_t *out_type) {
    vfs_lock();
    int result = vfs_stat_unlocked(node, out_size, out_type);
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
    // A node with its own operations (a device, /proc, a disk file) looks
    // after itself, and may wait a long time (a terminal read): the tree
    // lock is not held for it. An open node can not go away meanwhile.
    if (file && file->ops) return vfs_write_unlocked(file, offset, count, buf);
    vfs_lock();
    size_t result = vfs_write_unlocked(file, offset, count, buf);
    vfs_unlock();
    return result;
}
