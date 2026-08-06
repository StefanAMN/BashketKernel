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
    uint32_t mtime;    // Last written: FAT's packing, date << 16 | time (0: not known)
    uint32_t reserved;
    char name[128];
};

/**
 * @brief Set up the open-file allocator. Call once, before any file is opened.
 */
void file_init(void);

/**
 * @brief Open a path. On success *out holds a file with one reference.
 * @return 0 or a negative error number.
 */
int file_open(const char *path, int flags, file_t **out);

/**
 * @brief As file_open, with relative paths starting at `base` (NULL: the root).
 */
int file_open_at(vnode_t *base, const char *path, int flags, file_t **out);

/**
 * @brief As file_open_at, on behalf of user `uid`: the file's (and, when
 *        creating, the directory's) permissions must allow it, and a file
 *        created belongs to that user.
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
 * @brief Take another reference to an open file (fork, dup2).
 */
file_t *file_dup(file_t *file);

/**
 * @brief Drop a reference; the file is really closed when the last one goes.
 */
void file_close(file_t *file);

/**
 * @brief Close every descriptor in a table and clear it.
 */
void file_close_all(file_t *fds[TASK_MAX_FDS]);

/**
 * @brief Read/write at the file's position. May sleep (console, pipes).
 * @return Bytes transferred, 0 at end-of-file, or a negative error number.
 */
int64_t file_read(file_t *file, void *buf, size_t count);
int64_t file_write(file_t *file, const void *buf, size_t count);

/**
 * @brief Move the position of a regular file.
 * @return The new position or a negative error number.
 */
int64_t file_seek(file_t *file, int64_t offset, int whence);

/**
 * @brief Read the next entry of an open directory.
 * @return 1 if *out was filled, 0 at the end, or a negative error number.
 */
int file_readdir(file_t *file, struct dirent_info *out);

/**
 * @brief Put `file` in the lowest free slot of a descriptor table.
 * @return The descriptor, or -EMFILE if the table is full.
 */
int fd_install(file_t *fds[TASK_MAX_FDS], file_t *file);

#endif // FILE_H
