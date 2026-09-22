/**
 * @file file.c
 * @brief Open files and per-process file descriptor tables
 */

#include "file.h"
#include "heap.h"
#include "kstring.h"
#include "kerrno.h"
#include "tty.h"

static file_t *file_alloc(file_kind_t kind, bool readable, bool writable) {
    file_t *file = (file_t *)kmalloc(sizeof(file_t));
    if (!file) return NULL;
    memset(file, 0, sizeof(file_t));
    file->kind = kind;
    file->refs = 1;
    file->readable = readable;
    file->writable = writable;
    return file;
}

int file_open(const char *path, int flags, file_t **out) {
    return file_open_at(NULL, path, flags, out);
}

int file_open_at(vnode_t *base, const char *path, int flags, file_t **out) {
    if (!path || !out) return -EINVAL;

    int access = flags & O_ACCMODE;
    if (access == O_ACCMODE) return -EINVAL;
    bool writable = (access != O_RDONLY);

    vnode_t *node = vfs_find(base, path);
    if (!node && (flags & O_CREAT)) {
        const char *leaf = NULL;
        vnode_t *parent = vfs_find_parent_at(base, path, &leaf);
        if (!parent) return -ENOENT;
        node = vfs_create_file(parent, leaf, NULL, 0);
        if (!node) return -ENOMEM;
    }
    if (!node) return -ENOENT;
    if (node->type == VFS_DIRECTORY && writable) return -EISDIR;

    file_t *file = file_alloc(FILE_VNODE, access != O_WRONLY, writable);
    if (!file) return -ENOMEM;

    if (writable && (flags & O_TRUNC)) vfs_truncate(node, 0);
    file->append = (flags & O_APPEND) != 0;
    file->node = node;
    node->open_count++;

    *out = file;
    return 0;
}

file_t *file_open_console(void) {
    return file_alloc(FILE_CONSOLE, true, true);
}

int file_open_pipe(file_t **out_read, file_t **out_write) {
    pipe_t *pipe = pipe_create();
    file_t *reader = file_alloc(FILE_PIPE, true, false);
    file_t *writer = file_alloc(FILE_PIPE, false, true);

    if (!pipe || !reader || !writer) {
        kfree(pipe);
        kfree(reader);
        kfree(writer);
        return -ENOMEM;
    }

    reader->pipe = pipe;
    writer->pipe = pipe;
    *out_read = reader;
    *out_write = writer;
    return 0;
}

file_t *file_dup(file_t *file) {
    if (file) file->refs++;
    return file;
}

void file_close(file_t *file) {
    if (!file) return;
    if (--file->refs > 0) return;

    if (file->kind == FILE_VNODE && file->node) {
        file->node->open_count--;
    } else if (file->kind == FILE_PIPE) {
        if (file->readable) pipe_close_reader(file->pipe);
        else pipe_close_writer(file->pipe);
    }
    kfree(file);
}

void file_close_all(file_t *fds[TASK_MAX_FDS]) {
    for (int fd = 0; fd < TASK_MAX_FDS; fd++) {
        file_t *file = fds[fd];
        fds[fd] = NULL; // Clear the slot first: file_close may sleep or wake others
        file_close(file);
    }
}

int fd_install(file_t *fds[TASK_MAX_FDS], file_t *file) {
    for (int fd = 0; fd < TASK_MAX_FDS; fd++) {
        if (!fds[fd]) {
            fds[fd] = file;
            return fd;
        }
    }
    return -EMFILE;
}

int64_t file_read(file_t *file, void *buf, size_t count) {
    if (!file || !file->readable) return -EBADF;
    if (count == 0) return 0;

    switch (file->kind) {
        case FILE_CONSOLE:
            return tty_read(buf, count);
        case FILE_PIPE:
            return pipe_read(file->pipe, buf, count);
        case FILE_VNODE: {
            if (file->node->type == VFS_DIRECTORY) return -EISDIR;
            if (file->node->ops) {
                if (!file->node->ops->read) return 0;
                int64_t n = file->node->ops->read(file->node, file->offset, buf, count);
                if (n > 0) file->offset += (size_t)n;
                return n;
            }
            size_t n = vfs_read(file->node, file->offset, count, buf);
            file->offset += n;
            return (int64_t)n;
        }
    }
    return -EBADF;
}

int64_t file_write(file_t *file, const void *buf, size_t count) {
    if (!file || !file->writable) return -EBADF;
    if (count == 0) return 0;

    switch (file->kind) {
        case FILE_CONSOLE:
            return tty_write(buf, count);
        case FILE_PIPE:
            return pipe_write(file->pipe, buf, count);
        case FILE_VNODE: {
            if (file->node->ops) {
                if (!file->node->ops->write) return -EINVAL; // Read-only device
                int64_t n = file->node->ops->write(file->node, file->offset, buf, count);
                if (n > 0) file->offset += (size_t)n;
                return n;
            }
            if (file->append) file->offset = file->node->size;
            size_t n = vfs_write(file->node, file->offset, count, buf);
            if (n == 0) return -ENOSPC;
            file->offset += n;
            return (int64_t)n;
        }
    }
    return -EBADF;
}

int64_t file_seek(file_t *file, int64_t offset, int whence) {
    if (!file) return -EBADF;
    if (file->kind != FILE_VNODE || file->node->type != VFS_FILE) return -ESPIPE;

    int64_t base;
    switch (whence) {
        case SEEK_SET: base = 0; break;
        case SEEK_CUR: base = (int64_t)file->offset; break;
        case SEEK_END: base = (int64_t)file->node->size; break;
        default: return -EINVAL;
    }

    int64_t target = base + offset;
    if (target < 0 || target > (int64_t)1 << 30) return -EINVAL;

    file->offset = (size_t)target;
    return target;
}

int file_readdir(file_t *file, struct dirent_info *out) {
    if (!file || !file->readable || !out) return -EBADF;
    if (file->kind != FILE_VNODE || file->node->type != VFS_DIRECTORY) return -ENOTDIR;

    // The position is an entry index. Walking from the start each time keeps
    // this correct if entries are removed between calls.
    vnode_t *child = file->node->children;
    for (size_t i = 0; child && i < file->offset; i++) child = child->next;
    if (!child) return 0;

    memset(out, 0, sizeof(*out));
    out->type = child->type;
    out->size = child->size;
    strncpy(out->name, child->name, sizeof(out->name) - 1);
    file->offset++;
    return 1;
}
