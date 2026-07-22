/* A very small C library for BashketKernel user programs. */
#ifndef ULIB_H
#define ULIB_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include "syscall.h"

/* Process */
__attribute__((noreturn)) void exit(int status);
long fork(void);
long execve(const char *path, char *const argv[]);
long waitpid(long pid, int *status);
long getpid(void);
long getppid(void);
long kill(long pid, int sig);
void yield(void);
void sleep_ms(unsigned long ms);
long uptime_ticks(void);
void ignore_interrupt(int ignore);   /* 1: Ctrl-C does not end this process */

/* Memory */
void *sbrk(long increment);
void *mmap_anon(size_t length);
long munmap(void *addr, size_t length);
void *malloc(size_t size);
void free(void *ptr);

/* Files */
long open(const char *path, int flags);
long close(int fd);
long read(int fd, void *buf, size_t count);
long write(int fd, const void *buf, size_t count);
long lseek(int fd, long offset, int whence);
long pipe(int fds[2]);
long dup2(int old_fd, int new_fd);
long readdir(int fd, struct dirent *entry);   /* 1 = entry filled, 0 = end */
long mkdir(const char *path);
long unlink(const char *path);

/* Formatted output */
void puts(const char *s);
void printf(const char *fmt, ...);               /* To standard output */
void fprintf(int fd, const char *fmt, ...);
long readline(char *buf, size_t size);           /* -1 at end of input */
const char *strerror(long err);

/* Strings */
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *dst, const char *src);
char *strchr(const char *s, int c);
void *memset(void *dst, int value, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
int memcmp(const void *a, const void *b, size_t n);
long atoi(const char *s);

#endif
