/**
 * @file kerrno.h
 * @brief Error numbers. Kernel functions and system calls return them negated.
 *
 * The values match Linux so they look familiar; user/lib/syscall.h mirrors them.
 */

#ifndef KERRNO_H
#define KERRNO_H

#define EPERM    1   // Not allowed, whoever asks (short of root)
#define ENOENT   2   // No such file or directory
#define ESRCH    3   // No such process
#define EINTR    4   // A signal handler interrupted the call
#define E2BIG    7   // Argument list too long
#define EIO      5   // Input/output error
#define EBADF    9   // Bad file descriptor
#define ECHILD  10   // No child processes
#define ENOMEM  12   // Out of memory
#define EACCES  13   // The file's permissions do not allow it
#define EFAULT  14   // Bad address
#define EBUSY   16   // In use
#define EEXIST  17   // File exists
#define ENODEV  19   // No such device
#define ENOTDIR 20   // Not a directory
#define EXDEV   18   // Not the same filesystem
#define EISDIR  21   // Is a directory
#define EINVAL  22   // Invalid argument
#define EMFILE  24   // Too many open files
#define ETXTBSY 26   // A running program's file: not to be written, or a file open for writing: not to be run
#define EFBIG   27   // File too large
#define ENOSPC  28   // No space left
#define ESPIPE  29   // Illegal seek
#define EROFS   30   // Read-only filesystem
#define EPIPE   32   // Broken pipe
#define EMLINK  31   // Too many links to one file
#define ENAMETOOLONG 36
#define ELOOP   40   // Too many symbolic links in one path // A name longer than a directory entry can hold
#define ENOSYS  38
#define EMSGSIZE     90  // Message too long
#define EADDRINUSE   98  // Port already in use
#define ENETDOWN    100  // No network
#define ECONNRESET  104  // Connection reset by the other end
#define EISCONN     106  // Already connected
#define ENOTCONN    107  // Not connected
#define ETIMEDOUT   110  // Timed out
#define ECONNREFUSED 111 // Nobody listening on that port
#define EHOSTUNREACH 113 // No route to host   // Not implemented
#define ENOTEMPTY 39 // Directory not empty

#endif // KERRNO_H
