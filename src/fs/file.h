/**
 * @file file.h
 * @brief Open files and per-process file descriptor tables
 *
 * Three layers, as in Unix:
 *   - a file descriptor is a small integer, an index into the process's table;
 *   - each table slot points to an open file (file_t), which holds the read/
 *     write position and is shared by fork and dup2 (reference counted);
 *   - an open file refers to the thing itself: a VFS node, a pipe end, or the
 *     console.
 */

#ifndef FILE_H
#define FILE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "vfs.h"
#include "pipe.h"
#include "task.h"

// open() flags (values match Linux)
#define O_RDONLY  0x000
#define O_WRONLY  0x001
#define O_RDWR    0x002
#define O_ACCMODE 0x003
#define O_CREAT   0x040
#define O_TRUNC   0x200
#define O_APPEND  0x400
#define O_CLOEXEC 0x80000 // Close the descriptor when the process execs a new program

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

typedef enum {
    FILE_CONSOLE, // Keyboard/serial in, screen/serial out
    FILE_VNODE,   // A file or directory in the VFS
    FILE_PIPE,    // One end of a pipe (which end: readable or writable)
    FILE_SOCKET,  // A UDP socket: used with sendto/recvfrom, not read/write
    FILE_TCP      // A TCP socket: read and write once connected
} file_kind_t;

typedef struct file {
    file_kind_t kind;
    uint32_t refs;     // Descriptor table slots pointing here
    bool readable;
    bool writable;
    bool append;       // Every write goes to the end of the file
    vnode_t *node;     // FILE_VNODE
    size_t offset;     // FILE_VNODE: position; for a directory, the entry index
    char *snapshot;    // FILE_VNODE with a snapshot operation (/proc): the text reads come from (kmalloc), or NULL
    size_t snapshot_len;
    pipe_t *pipe;      // FILE_PIPE
    struct udp_socket *socket; // FILE_SOCKET
    struct tcp_socket *tcp;    // FILE_TCP
} file_t;

// One directory entry, as returned to user programs by readdir
struct dirent_info {
    uint32_t type;     // VFS_FILE or VFS_DIRECTORY
    uint32_t mode;     // Permission bits (see auth.h)
    uint64_t size;
    uint32_t uid;      // Owner
    uint32_t gid;      // Group
    uint32_t mtime;    // Last written: seconds since 1970, UTC (0: not known)
    uint32_t reserved;
    char name[128];
};

/**
 * @brief Set up the open-file allocator. Call once, before any file is opened.
 */
void file_init(void);

/**
 * @brief Open a path for the kernel itself (no permission checks; relative
 *        paths start at the root). On success *out holds a file with one
 *        reference. `flags`: O_RDONLY, O_WRONLY or O_RDWR, with O_CREAT,
 *        O_TRUNC, O_APPEND.
 * @return 0 or a negative error number (ENOENT, EISDIR to write a
 *         directory, EROFS, EACCES, ENOMEM, or the device's refusal).
 */
int file_open(const char *path, int flags, file_t **out);

/**
 * @brief As file_open, on behalf of user `uid` and with relative paths
 *        starting at `base` (NULL: the root): the file's (and, when
 *        creating, the directory's) permissions must allow it, and a file
 *        created belongs to that user. O_CREAT of "name/" creates "name".
 *        Takes the tree lock; may sleep (a directory read from a disk, a
 *        device's `opened`).
 */
int file_open_as(uint32_t uid, vnode_t *base, const char *path, int flags, file_t **out);

/**
 * @brief A new open file for the console (readable and writable).
 */
file_t *file_open_console(void);

/**
 * @brief Create a pipe and an open file for each end.
 * @return 0 or a negative error number.
 */
int file_open_pipe(file_t **out_read, file_t **out_write);

/**
 * @brief A new open file for a new UDP socket (NULL if none can be made).
 */
file_t *file_open_socket(void);

/**
 * @brief An open file for a TCP socket: a new one (tcp == NULL), or one that
 *        tcp_accept returned. Takes the socket over: it is closed with the
 *        file, and also if this fails.
 */
file_t *file_open_tcp(struct tcp_socket *tcp);

/**
 * @brief fstat(): describe an open file. A file or directory as vfs_stat
 *        does (the console's node as the terminal); a pipe, socket or the
 *        terminal by its type alone (no owner, size 0, mode 0600).
 * @return 0, -EBADF for NULL, or vfs_stat's error.
 */
int file_stat(file_t *file, struct vfs_stat *out);

/**
 * @brief Whether the file is the terminal (the console, opened at boot or
 *        as /dev/console): what isatty() asks.
 */
bool file_is_terminal(const file_t *file);

/**
 * @brief Take another reference to an open file (fork, dup2).
 */
file_t *file_dup(file_t *file);

/**
 * @brief Drop a reference; the file is really closed when the last one goes
 *        (the node's open count drops, a pipe end or socket is closed, a
 *        /proc text kept for it is freed).
 *        May sleep: call without spinlocks held.
 */
void file_close(file_t *file);

/**
 * @brief Close every descriptor in a table and clear it (each slot is
 *        emptied before its file is closed). May sleep.
 */
void file_close_all(file_t *fds[TASK_MAX_FDS]);

/**
 * @brief Read up to `count` bytes at the file's position into the kernel
 *        buffer `buf`, and move the position past them. May sleep (console,
 *        pipes, sockets, disks). A /proc file (a node with a snapshot
 *        operation) is read from a copy of its text taken at the first
 *        read and at every read from position 0, and kept until close, so
 *        reads in small pieces fit together.
 * @return Bytes read, 0 at end-of-file, or a negative error number (EBADF
 *         if not open for reading, EISDIR for a directory, EINVAL for a UDP
 *         socket, or the device's error).
 */
int64_t file_read(file_t *file, void *buf, size_t count);

/**
 * @brief Write `count` bytes from the kernel buffer `buf` at the file's
 *        position (at its end with O_APPEND), and move the position past
 *        them. May sleep.
 * @return Bytes written or a negative error number (EBADF, ENOSPC when a
 *         file kept in memory cannot grow, EPIPE, the device's error).
 */
int64_t file_write(file_t *file, const void *buf, size_t count);

/**
 * @brief Move the position of a regular file: to `offset` from the start
 *        (SEEK_SET), the current position (SEEK_CUR) or the end (SEEK_END).
 * @return The new position, or -ESPIPE for anything but a file, -EINVAL
 *         for a position below 0 or above 1 GiB.
 */
int64_t file_seek(file_t *file, int64_t offset, int whence);

/**
 * @brief Read the next entry of an open directory.
 * @return 1 if *out was filled, 0 at the end, or a negative error number.
 */
int file_readdir(file_t *file, struct dirent_info *out);

/**
 * @brief Store `file` (or NULL) in slot `fd` of a descriptor table and
 *        return what was there, under the lock that lets /proc read other
 *        processes' tables (fd_slot_dup). Every emptying or replacing of a
 *        slot goes through this; close the returned file afterwards.
 */
file_t *fd_slot_set(file_t *fds[TASK_MAX_FDS], int fd, file_t *file);

/**
 * @brief A reference to the file in slot `fd` of any process's table
 *        (file_close it when done), or NULL if the slot is empty or out
 *        of range. For another process's table the caller keeps the task
 *        alive: it holds the scheduler lock (this takes a spinlock ranked
 *        above it, and does not sleep).
 */
file_t *fd_slot_dup(file_t *fds[TASK_MAX_FDS], int fd);

/**
 * @brief Which slots of a table are in use, bit n for descriptor n: a
 *        moment's picture, for listing them. Does not sleep.
 */
uint32_t fd_slot_mask(file_t *fds[TASK_MAX_FDS]);

/**
 * @brief Put `file` in the lowest free slot of a descriptor table.
 * @return The descriptor, or -EMFILE if the table is full.
 */
int fd_install(file_t *fds[TASK_MAX_FDS], file_t *file);

#endif // FILE_H
