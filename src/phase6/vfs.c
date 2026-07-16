/**
 * @file vfs.c
 * @brief Implementation of Virtual Filesystem (VFS) Tree Management
 */

#include "vfs.h"
#include "heap.h"
#include "kprintf.h"
#include "string_util.h"

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

    size_t new_size = offset + count;
    uint8_t *new_data = (uint8_t *)kmalloc(new_size);
    if (!new_data) return 0;

    if (file->data && file->size > 0) {
        size_t copy_len = (file->size < offset) ? file->size : offset;
        memcpy(new_data, file->data, copy_len);
    }

    memcpy(new_data + offset, buf, count);

    file->data = new_data;
    if (new_size > file->size) {
        file->size = new_size;
    }

    return count;
}

int vfs_unlink(vnode_t *parent, const char *name) {
    if (!parent) parent = vfs_root;
    if (!name || parent->type != VFS_DIRECTORY) return -1;

    vnode_t *curr = parent->children;
    vnode_t *prev = NULL;

    while (curr) {
        if (strcmp(curr->name, name) == 0) {
            if (prev) {
                prev->next = curr->next;
            } else {
                parent->children = curr->next;
            }
            kfree(curr);
            return 0;
        }
        prev = curr;
        curr = curr->next;
    }

    return -1; // File not found
}

int vfs_stat(vnode_t *node, size_t *out_size, uint32_t *out_type) {
    if (!node) return -1;
    if (out_size) *out_size = node->size;
    if (out_type) *out_type = node->type;
    return 0;
}
