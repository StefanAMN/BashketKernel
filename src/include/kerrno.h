/**
 * @file kerrno.h
 * @brief Error numbers. Kernel functions and system calls return them negated.
 *
 * The values match Linux so they look familiar; user/lib/syscall.h mirrors them.
 */

#ifndef KERRNO_H
#define KERRNO_H

#define ENOENT   2   // No such file or directory
#define ESRCH    3   // No such process
#define E2BIG    7   // Argument list too long
#define EBADF    9   // Bad file descriptor
#define ECHILD  10   // No child processes
#define ENOMEM  12   // Out of memory
#define EFAULT  14   // Bad address
#define EBUSY   16   // In use
#define EEXIST  17   // File exists
#define ENOTDIR 20   // Not a directory
#define EISDIR  21   // Is a directory
#define EINVAL  22   // Invalid argument
#define EMFILE  24   // Too many open files
#define ENOSPC  28   // No space left
#define ESPIPE  29   // Illegal seek
#define EPIPE   32   // Broken pipe
#define ENOSYS  38   // Not implemented
#define ENOTEMPTY 39 // Directory not empty

#endif // KERRNO_H
