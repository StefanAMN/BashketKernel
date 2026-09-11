/**
 * @file vfs.c
 * @brief Implementation of Virtual Filesystem (VFS) Tree Management
 */

#include "vfs.h"
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
    vfs_root->parent = NULL;
    vfs_root->children = NULL;
    vfs_root->next = NULL;

    kprintf("VFS Init: Root directory ('/') created successfully.\n");
}

vnode_t *vfs_get_root(void) {
    return vfs_root;
}

vnode_t *vfs_find_child(vnode_t *dir, const char *name) {
    if (!dir || dir->type != VFS_DIRECTORY || !name) return NULL;
    for (vnode_t *child = dir->children; child; child = child->next) {
        if (strcmp(child->name, name) == 0) return child;
    }
    return NULL;
}

vnode_t *vfs_find_parent(const char *path, const char **out_leaf) {
    if (!path || !out_leaf) return NULL;

    // Find the last '/' that is followed by something
    const char *leaf = path;
    for (const char *p = path; *p != '\0'; p++) {
        if (*p == '/' && p[1] != '\0') leaf = p + 1;
    }
    if (*leaf == '\0' || *leaf == '/') return NULL;

    // Everything before the leaf names the parent
    size_t parent_len = (size_t)(leaf - path);
    char parent_path[128];
    if (parent_len >= sizeof(parent_path)) return NULL;
    memcpy(parent_path, path, parent_len);
    parent_path[parent_len] = '\0';

    // Drop the separator, but keep a lone "/" meaning the root
    while (parent_len > 1 && parent_path[parent_len - 1] == '/') parent_path[--parent_len] = '\0';

    vnode_t *parent = (parent_len == 0) ? vfs_root : vfs_find(NULL, parent_path);
    if (!parent || parent->type != VFS_DIRECTORY) return NULL;

    *out_leaf = leaf;
    return parent;
}

vnode_t *vfs_create_dir(vnode_t *parent, const char *name) {
    if (!parent) parent = vfs_root;
    if (!name || parent->type != VFS_DIRECTORY) return NULL;

    // Check if directory already exists
    vnode_t *curr = parent->children;
    while (curr) {
        if (strcmp(curr->name, name) == 0) {
            return curr;
        }
        curr = curr->next;
    }

    vnode_t *dir = (vnode_t *)kmalloc(sizeof(vnode_t));
    if (!dir) return NULL;
    memset(dir, 0, sizeof(vnode_t));

    size_t i = 0;
    for (; name[i] != '\0' && i < sizeof(dir->name) - 1; i++) {
        dir->name[i] = name[i];
    }
    dir->name[i] = '\0';

    dir->type = VFS_DIRECTORY;
    dir->parent = parent;
    dir->next = parent->children;
    parent->children = dir;

    return dir;
}

vnode_t *vfs_create_file(vnode_t *parent, const char *name, const void *data, size_t size) {
    if (!parent) parent = vfs_root;
    if (!name || parent->type != VFS_DIRECTORY) return NULL;

    if (name[0] == '\0' || vfs_find_child(parent, name)) return NULL; // Empty or taken

    vnode_t *file = (vnode_t *)kmalloc(sizeof(vnode_t));
    if (!file) return NULL;
    memset(file, 0, sizeof(vnode_t));

    size_t i = 0;
    for (; name[i] != '\0' && i < sizeof(file->name) - 1; i++) {
        file->name[i] = name[i];
    }
    file->name[i] = '\0';

    file->type = VFS_FILE;
    file->size = size;
    file->data = data;
    file->parent = parent;

    file->next = parent->children;
    parent->children = file;

    return file;
}

vnode_t *vfs_find(vnode_t *relative_root, const char *path) {
    if (!path) return NULL;
    if (!relative_root || path[0] == '/') {
        relative_root = vfs_root;
    }

    if (strcmp(path, "/") == 0) return vfs_root;

    // Skip leading slash
    if (path[0] == '/') path++;

    char token[128];
    vnode_t *curr_dir = relative_root;

    while (*path != '\0') {
        size_t len = 0;
        while (path[len] != '\0' && path[len] != '/') {
            if (len < sizeof(token) - 1) {
                token[len] = path[len];
            }
            len++;
        }
        token[len] = '\0';

        path += len;
        if (*path == '/') path++;

        // Search current directory children for token
        vnode_t *child = curr_dir->children;
        vnode_t *found = NULL;
        while (child) {
            if (strcmp(child->name, token) == 0) {
                found = child;
                break;
            }
            child = child->next;
        }

        if (!found) return NULL;

        if (*path == '\0') {
            return found; // Reached target node
        }

        if (found->type != VFS_DIRECTORY) return NULL;
        curr_dir = found;
    }

    return NULL;
}

size_t vfs_read(vnode_t *file, size_t offset, size_t count, void *buf) {
    if (!file || file->type != VFS_FILE || !buf || !file->data) return 0;
    if (offset >= file->size) return 0;

    size_t available = file->size - offset;
    size_t to_read = (count < available) ? count : available;

    const uint8_t *src = (const uint8_t *)file->data + offset;
    memcpy(buf, src, to_read);
    return to_read;
}

size_t vfs_write(vnode_t *file, size_t offset, size_t count, const void *buf) {
    if (!file || file->type != VFS_FILE || !buf || count == 0) return 0;

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

void vfs_truncate(vnode_t *file, size_t size) {
    if (!file || file->type != VFS_FILE) return;
    if (size < file->size) file->size = size;
}

int vfs_unlink(vnode_t *parent, const char *name) {
    if (!parent) parent = vfs_root;
    if (!name || parent->type != VFS_DIRECTORY) return -ENOTDIR;

    vnode_t *prev = NULL;
    for (vnode_t *curr = parent->children; curr; prev = curr, curr = curr->next) {
        if (strcmp(curr->name, name) != 0) continue;

        if (curr->children) return -ENOTEMPTY;
        if (curr->open_count > 0) return -EBUSY; // Someone holds it open

        if (prev) prev->next = curr->next; else parent->children = curr->next;
        if (curr->owns_data) kfree((void *)curr->data);
        kfree(curr);
        return 0;
    }

    return -ENOENT;
}

int vfs_stat(vnode_t *node, size_t *out_size, uint32_t *out_type) {
    if (!node) return -1;
    if (out_size) *out_size = node->size;
    if (out_type) *out_type = node->type;
    return 0;
}
