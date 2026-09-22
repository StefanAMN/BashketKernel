/**
 * @file file.c
 * @brief Open files and per-process file descriptor tables
 */

#include "file.h"
#include "spinlock.h"
#include "heap.h"
#include "kstring.h"
#include "kerrno.h"
#include "tty.h"
#include "slab.h"
#include "net.h"
#include "tcp.h"
#include "auth.h"
#include "dev.h"

// Open files are small, all the same size, and created and destroyed
// constantly (every open, pipe and exit), which is what a slab cache is for.
static kmem_cache_t *file_cache = NULL;

void file_init(void) {
    if (file_cache) return;
    file_cache = kmem_cache_create("file", sizeof(file_t), 8);

    // Take the cache's first page now, so that later "nothing leaked" checks
    // that count free frames are not thrown off by the first file ever opened
    kmem_cache_free(file_cache, kmem_cache_alloc(file_cache));
}

// A new open file of `kind` with one reference and position 0, or NULL
static file_t *file_alloc(file_kind_t kind, bool readable, bool writable) {
    file_t *file = (file_t *)kmem_cache_alloc(file_cache);
    if (!file) return NULL;
    memset(file, 0, sizeof(file_t));
    file->kind = kind;
    file->refs = 1;
    file->readable = readable;
    file->writable = writable;
    return file;
}

// Give an open file's memory back (NULL is allowed). Closes nothing.
static void file_free(file_t *file) {
    kmem_cache_free(file_cache, file);
}

int file_open(const char *path, int flags, file_t **out) {
    return file_open_as(UID_ROOT, NULL, path, flags, out); // The kernel itself is not restricted
}

static int open_as_locked(uint32_t uid, vnode_t *base, const char *path, int flags, file_t **out);

int file_open_as(uint32_t uid, vnode_t *base, const char *path, int flags, file_t **out) {
    // From looking the name up until the node is pinned by its open count,
    // nothing may remove it
    vfs_lock();
    int result = open_as_locked(uid, base, path, flags, out);
    vfs_unlock();
    return result;
}

// See file_open_as. Caller holds the tree lock, which keeps the node from
// being removed between the lookup and the open count going up.
static int open_as_locked(uint32_t uid, vnode_t *base, const char *path, int flags, file_t **out) {
    if (!path || !out) return -EINVAL;

    int access = flags & O_ACCMODE;
    if (access == O_ACCMODE) return -EINVAL;
    bool writable = (access != O_RDONLY);

    // (Symbolic links are followed; the lookup says why it failed: a loop of
    // links is ELOOP, a file used as a directory ENOTDIR)
    vnode_t *node = NULL;
    int looked = vfs_lookup_as(uid, base, path, true, &node);
    if (looked < 0 && looked != -ENOENT) return looked;
    if (!node && (flags & O_CREAT)) {
        char leaf[VFS_NAME_MAX];
        vnode_t *parent = NULL;
        int found = vfs_find_parent_as(uid, base, path, &parent, leaf);
        if (found < 0) return found;
        if (!auth_may_access(uid, parent, true)) return -EACCES; // Creating changes the directory
        int err = vfs_create(parent, leaf, VFS_FILE, &node);
        if (err != 0) return err;
        // The new file belongs to whoever made it, and to their group, on a
        // filesystem that can record that. (One that cannot, like FAT, has
        // set what it shows for every file.)
        if (!parent->ops || !parent->ops->create || (node->ops && node->ops->setattr)) {
            node->uid = uid;
            node->gid = auth_new_entry_group(parent, (uid == auth_current_uid()) ? auth_current_gid() : GID_ROOT);
            node->mode = (uint16_t)(PERM_NEW_FILE & ~auth_current_umask());
            if (node->ops && node->ops->setattr) node->ops->setattr(node);
        }
    }
    if (!node) return -ENOENT;
    if (node->type == VFS_DIRECTORY && writable) return -EISDIR;
    if (node->read_only && writable) return -EROFS;
    if (access != O_WRONLY && !auth_may_access(uid, node, false)) return -EACCES;
    if (writable && !auth_may_access(uid, node, true)) return -EACCES;
    // A running program reads its file page by page: changing it would
    // change code not read yet under the program's feet
    if (writable && node->exec_count > 0) return -ETXTBSY;

    file_t *file = file_alloc(FILE_VNODE, access != O_WRONLY, writable);
    if (!file) return -ENOMEM;

    if (writable && (flags & O_TRUNC)) vfs_truncate(node, 0);
    // A set-uid (or set-gid) program that anyone but root can rewrite would
    // be a way to become its owner (or join its group): opening it for
    // writing takes the bits away
    if (writable && uid != UID_ROOT && (node->mode & PERM_SET_IDS)) {
        node->mode &= (uint16_t)~PERM_SET_IDS;
        if (node->ops && node->ops->setattr) node->ops->setattr(node);
    }
    if (node->ops && node->ops->opened) {
        int refused = node->ops->opened(node, writable);
        if (refused < 0) {
            file_free(file);
            return refused;
        }
    }
    file->append = (flags & O_APPEND) != 0;
    file->node = node;
    node->open_count++;
    if (writable) node->write_count++;   // (Until closed, the file cannot be run: see process.c)

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
        file_free(reader);
        file_free(writer);
        return -ENOMEM;
    }

    reader->pipe = pipe;
    writer->pipe = pipe;
    *out_read = reader;
    *out_write = writer;
    return 0;
}

file_t *file_open_socket(void) {
    udp_socket_t *socket = udp_open();
    if (!socket) return NULL;

    file_t *file = file_alloc(FILE_SOCKET, true, true);
    if (!file) {
        udp_close(socket);
        return NULL;
    }
    file->socket = socket;
    return file;
}

file_t *file_open_tcp(struct tcp_socket *tcp) {
    if (!tcp) tcp = tcp_open();
    if (!tcp) return NULL;
    file_t *file = file_alloc(FILE_TCP, true, true);
    if (!file) {
        tcp_close(tcp);
        return NULL;
    }
    file->tcp = tcp;
    return file;
}

// An open file can be shared by processes on different CPUs (fork copies
// the descriptor table), so the count of holders is changed atomically
int file_stat(file_t *file, struct vfs_stat *out) {
    if (!file || !out) return -EBADF;
    if (file->kind == FILE_VNODE) {
        int err = vfs_stat(file->node, out);
        if (err == 0 && dev_is_console(file->node)) out->type = STAT_TERMINAL;
        return err;
    }
    memset(out, 0, sizeof(*out));
    out->links = 1;
    out->mode = 0600;                                      // Its holders may read and write it
    out->type = file->kind == FILE_PIPE ? STAT_PIPE : file->kind == FILE_CONSOLE ? STAT_TERMINAL : STAT_SOCKET;
    return 0;
}

bool file_is_terminal(const file_t *file) {
    if (!file) return false;
    return file->kind == FILE_CONSOLE || (file->kind == FILE_VNODE && dev_is_console(file->node));
}

file_t *file_dup(file_t *file) {
    if (file) __atomic_add_fetch(&file->refs, 1, __ATOMIC_SEQ_CST);
    return file;
}

void file_close(file_t *file) {
    if (!file) return;
    if (__atomic_sub_fetch(&file->refs, 1, __ATOMIC_ACQ_REL) > 0) return;

    if (file->kind == FILE_VNODE && file->node) {
        // The open count belongs to the tree lock, like every other change
        // to it (another CPU may be opening the same node)
        vfs_lock();
        file->node->open_count--;
        if (file->writable) file->node->write_count--;
        if (file->node->ops && file->node->ops->closed) file->node->ops->closed(file->node, file->writable);
        vfs_unlock();
    } else if (file->kind == FILE_PIPE) {
        if (file->readable) pipe_close_reader(file->pipe);
        else pipe_close_writer(file->pipe);
    } else if (file->kind == FILE_SOCKET) {
        udp_close(file->socket);
    } else if (file->kind == FILE_TCP) {
        tcp_close(file->tcp);
    }
    file_free(file);
}

void file_close_all(file_t *fds[TASK_MAX_FDS]) {
    for (int fd = 0; fd < TASK_MAX_FDS; fd++) {
        // Clear the slot first: file_close may sleep or wake others
        file_close(fd_slot_set(fds, fd, NULL));
    }
}

// Held while a descriptor slot is emptied or replaced, and while another
// process's slot is read and its file given a reference (fd_slot_dup). A
// file in a slot holds a reference of its own, so under this lock a file
// seen in a slot cannot reach its last file_close: the slot must be
// emptied first, and that waits for the lock. (Filling an empty slot needs
// no lock: the file is complete and counted before it is stored.)
static spinlock_t fd_slot_lock = SPINLOCK_RANKED("fd-slots", LOCK_RANK_FD_SLOTS);

file_t *fd_slot_set(file_t *fds[TASK_MAX_FDS], int fd, file_t *file) {
    uint64_t flags = spinlock_acquire_irqsave(&fd_slot_lock);
    file_t *old = fds[fd];
    fds[fd] = file;
    spinlock_release_irqrestore(&fd_slot_lock, flags);
    return old;
}

file_t *fd_slot_dup(file_t *fds[TASK_MAX_FDS], int fd) {
    if (fd < 0 || fd >= TASK_MAX_FDS) return NULL;
    uint64_t flags = spinlock_acquire_irqsave(&fd_slot_lock);
    file_t *file = file_dup(fds[fd]);
    spinlock_release_irqrestore(&fd_slot_lock, flags);
    return file;
}

uint32_t fd_slot_mask(file_t *fds[TASK_MAX_FDS]) {
    uint32_t mask = 0;
    for (int fd = 0; fd < TASK_MAX_FDS; fd++) {
        if (__atomic_load_n(&fds[fd], __ATOMIC_RELAXED)) mask |= 1u << fd;
    }
    return mask;
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
        case FILE_SOCKET:
            return -EINVAL; // Datagrams have addresses: use recvfrom
        case FILE_TCP:
            return tcp_read(file->tcp, buf, count);
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
        case FILE_SOCKET:
            return -EINVAL; // ... and sendto
        case FILE_TCP:
            return tcp_write(file->tcp, buf, count);
        case FILE_VNODE: {
            if (file->node->ops) {
                if (!file->node->ops->write) return -EINVAL; // Read-only device
                if (file->append) file->offset = file->node->size;
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

static int readdir_locked(file_t *file, struct dirent_info *out);

int file_readdir(file_t *file, struct dirent_info *out) {
    vfs_lock(); // The list of children must not change while it is walked
    int result = readdir_locked(file, out);
    vfs_unlock();
    return result;
}

// See file_readdir. Caller holds the tree lock.
static int readdir_locked(file_t *file, struct dirent_info *out) {
    if (!file || !file->readable || !out) return -EBADF;
    if (file->kind != FILE_VNODE || file->node->type != VFS_DIRECTORY) return -ENOTDIR;

    // The position is an entry index. Walking from the start each time keeps
    // this correct if entries are removed between calls.
    vfs_load_dir(file->node);
    vnode_t *child = file->node->children;
    for (size_t i = 0; child && i < file->offset; i++) child = child->next;
    if (!child) return 0;

    memset(out, 0, sizeof(*out));
    out->type = child->type;
    out->mode = child->mode;
    out->uid = child->uid;
    out->gid = child->gid;
    out->mtime = child->mtime;
    out->size = child->size;
    strncpy(out->name, child->name, sizeof(out->name) - 1);
    file->offset++;
    return 1;
}
