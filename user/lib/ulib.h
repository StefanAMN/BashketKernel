/* A very small C library for BashketKernel user programs. */
#ifndef ULIB_H
#define ULIB_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdbool.h>
#include "syscall.h"

/* Process */
__attribute__((noreturn)) void exit(int status);
long fork(void); /* A copy of this process; returns the child's pid here, 0 in the child, or a negative error */
long execve(const char *path, char *const argv[], char *const envp[]); /* Run another program in this process; returns only on error */
long execv(const char *path, char *const argv[]);    /* Passes on our own environment */

/* Environment: "NAME=value" strings, inherited across fork and exec */
extern char **environ;
const char *getenv(const char *name); /* The value of NAME in the environment, or NULL */
int setenv(const char *name, const char *value);     /* 0, or -1 if out of memory */
long waitpid(long pid, int *status, int options);  /* pid -1: any child. options: 0, or WNOHANG */
long getpid(void); /* This process's id */
long getppid(void); /* Its parent's id */
long kill(long pid, int sig); /* Send a signal to a process (negative pid: to its whole group) */
void yield(void); /* Let another task run */
void sleep_ms(unsigned long ms); /* Sleep for at least `ms` milliseconds */
long uptime_ticks(void);                     /* Timer ticks (10 ms each) since boot */
unsigned long clock_ns(void);                /* Nanoseconds since boot */
unsigned long time_ns(void);                 /* The time of day: nanoseconds since 1970-01-01 00:00 UTC */
long set_time(unsigned long seconds);        /* Set the time of day (root only): 0, -EPERM, or -EINVAL outside 1980..2105 */
long set_time_ns(unsigned long ns);          /* The same, to the nanosecond */
void format_date(unsigned long seconds, char out[20]); /* Seconds since 1970 -> "2026-10-20 14:03:07" (UTC) */
long parse_date(const char *text, unsigned long *seconds); /* "YYYY-MM-DD HH:MM[:SS]" -> seconds since 1970; 0 or -EINVAL */
long sleep_ns(unsigned long nanoseconds);    /* Sleeps to within microseconds; -EINTR if a signal cut it short */
long set_priority(int priority);    /* 0 high, 1 normal, 2 low */
void ignore_interrupt(int ignore);   /* 1: Ctrl-C does not end this process */

/* Signals */
#define SIGINT   2
#define SIGILL   4                  /* Killed for an unknown instruction (cannot be caught yet) */
#define SIGFPE   8                  /* Killed for a division by zero (cannot be caught yet) */
#define SIGKILL  9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGTERM 15
#define SIGCHLD 17                  /* A child has exited or stopped; ignored unless a handler is set */
#define SIGCONT 18                  /* Continue a stopped process */
#define SIGSTOP 19                  /* Stop (suspend) a process; it has no say */
#define SIGTSTP 20                  /* Stop, from the terminal (Ctrl-Z); may be caught or ignored */
typedef void (*sighandler_t)(int);
#define SIG_DFL ((sighandler_t)0)   /* End the process */
#define SIG_IGN ((sighandler_t)1)   /* Do nothing */
#define SIG_ERR ((sighandler_t)-1)
/* Choose what a signal does; returns the previous choice or SIG_ERR.
 * If the process is asleep in a system call when a handled signal arrives,
 * that call returns -EINTR after the handler has run; call it again if the
 * work still needs doing. */
sighandler_t signal(int sig, sighandler_t handler);

/* The full form. While a handler runs, its own signal and those in `mask`
 * are held back. With SA_RESTART, a read, write, waitpid or accept that the
 * signal interrupts is made again after the handler instead of failing with
 * -EINTR. Either pointer may be NULL. */
struct sigaction {
    sighandler_t handler;
    uint32_t mask;                  /* A set of signals: sigbit(SIGUSR1) | sigbit(SIGUSR2) */
    uint32_t flags;                 /* 0 or SA_RESTART */
};
#define SA_RESTART 1
#define sigbit(sig) (1u << (sig))
long sigaction(int sig, const struct sigaction *action, struct sigaction *old); /* Returns 0 or a negative error */

/* Hold signals back: a blocked signal waits until it is unblocked. SIGKILL
 * can not be blocked. Either pointer may be NULL (old alone reads the set). */
#define SIG_BLOCK   0               /* Add `set` to the blocked signals */
#define SIG_UNBLOCK 1               /* Remove it */
#define SIG_SETMASK 2               /* Replace them with it */
long sigprocmask(int how, const uint32_t *set, uint32_t *old); /* Returns 0 or a negative error */

/* Jobs. A process group is the set of processes a shell started for one
 * command; kill() with a negative number signals a whole group. The
 * terminal's foreground group is the one Ctrl-C and Ctrl-Z act on. */
long setpgid(long pid, long pgid);           /* pid 0: the caller; pgid 0: a group of its own */
long getpgid(long pid); /* The process group of `pid` (0: the caller) */
long tcsetpgrp(long pgid);                   /* 0: no job control (Ctrl-C reaches every process) */
long tcgetpgrp(void); /* The terminal's foreground group (0: none) */

/* Users and permissions. uid 0 is root and may do anything. */
long getuid(void);                           /* The real user: who started this process */
long geteuid(void);                          /* The effective user: whose rights it has (differs in a set-uid program) */
long getgid(void); /* The group this process runs as */
long setuid(unsigned uid);                   /* Root may become anyone; anyone else may only go back to their real user */
long setgid(unsigned gid);                   /* Root only */
long umask(unsigned mask);                   /* Bits removed from the mode of files this process creates; returns the old mask */
long chmod(const char *path, unsigned mode); /* Octal: owner, group, others; 4 read, 2 write, 1 execute. 04000: set-uid */
long chown(const char *path, unsigned uid, unsigned gid); /* Root only (an owner may set their own group); KEEP_ID leaves one as it is */
#define KEEP_ID 0xFFFFFFFFu

/* Memory */
void *sbrk(long increment); /* Move the end of the heap; returns the old end, or (void *)-1 */
void *mmap_anon(size_t length);             /* Zero-filled memory. A page costs nothing until it is first touched. */
void *mmap_file(int fd, size_t length, size_t offset, int prot); /* The file's contents as memory, read when first touched; a private copy: writing to it does not change the file */
void *mmap_shared(size_t length);            /* Stays shared with children created by fork */

/* Shared memory between any processes that agree on a key */
long shmget(unsigned key, size_t size);      /* Find or create; returns an id */
void *shmat(long id);                        /* Map it here; NULL on failure. munmap() to detach. */
long shmrm(long id);                         /* Release the key; memory goes with the last mapping */
long munmap(void *addr, size_t length); /* Give back pages from mmap_* or shmat */
void *malloc(size_t size); /* Heap memory (16-byte aligned), or NULL (also for 0 and absurd sizes) */
void free(void *ptr); /* Give back what malloc returned (NULL is allowed) */

/* Files */
long open(const char *path, int flags); /* A descriptor, or a negative error. flags: O_RDONLY, O_WRONLY or O_RDWR, with O_CREAT, O_TRUNC, O_APPEND, O_CLOEXEC */
long close(int fd); /* 0, or -EBADF */
long read(int fd, void *buf, size_t count); /* Bytes read (0: end of file), or a negative error */
long write(int fd, const void *buf, size_t count); /* Bytes written, or a negative error */
long lseek(int fd, long offset, int whence); /* New position (SEEK_SET, SEEK_CUR, SEEK_END), or a negative error */
long pipe(int fds[2]); /* fds[0] reads what fds[1] writes */
long dup2(int old_fd, int new_fd); /* Make new_fd refer to old_fd's open file (closing what it was) */
long dup(int fd);                            /* The same open file under the lowest free descriptor */
long pipe2(int fds[2], int flags);           /* flags: 0 or O_CLOEXEC */
long fcntl(int fd, int command, long arg);   /* F_GETFD, or F_SETFD with FD_CLOEXEC or 0 */
long readdir(int fd, struct dirent *entry);   /* 1 = entry filled, 0 = end */
long poll(struct pollfd *fds, unsigned count, long timeout_ms); /* Wait until one is ready (-1: no limit; 0: just look). How many are; 0 on timeout; -EINTR */
long mkdir(const char *path); /* 0, or a negative error (-EEXIST, -ENOENT, -EACCES, -ENAMETOOLONG ...) */
long chdir(const char *path); /* Change the working directory */
long getcwd(char *buf, size_t size);          /* Length of the path, or a negative error */
long unlink(const char *path);
long symlink(const char *target, const char *path); /* Make path a symbolic link to target */
long link(const char *existing, const char *path);  /* A second name for a file (on the ext2 disk only) */
long readlink(const char *path, char *buf, size_t size); /* A link's target (not NUL-terminated): its length, or -EINVAL if not a link */ /* Remove a file or an empty directory */
long rename(const char *from, const char *to);  /* Within one filesystem; fails with -EEXIST rather than replace anything */

/* Network. The machine is 10.0.2.15; QEMU's gateway (which also reaches the
 * host computer) is 10.0.2.2.
 *
 * UDP (SOCK_DGRAM): bind, sendto, recvfrom.
 * TCP (SOCK_STREAM): a client calls connect; a server calls bind, listen and
 * then accept for each connection. A connection is read and written like a
 * file; read returns 0 when the other end has closed. */
long socket(int type);                            /* A new socket, as a file descriptor */
long bind(int fd, unsigned port);                 /* The port to receive on (UDP) or to listen on (TCP) */
long connect(int fd, uint32_t ip, unsigned port); /* TCP: open a connection; waits until it is made */
long listen(int fd, int backlog);                 /* TCP: take connections on the bound port */
long shutdown(int fd);                             /* TCP: finished writing; the other end reads end-of-file. Reading goes on */
long accept(int fd, struct net_address *from);    /* TCP: the next connection, as a new descriptor (from may be NULL) */
long sendto(int fd, const void *buf, size_t len, uint32_t ip, unsigned port); /* UDP: one datagram; bytes sent, or a negative error */
long recvfrom(int fd, void *buf, size_t len, struct net_address *from, unsigned long timeout_ms); /* 0: wait for ever */
long ping(uint32_t ip, unsigned long timeout_ms); /* Round-trip time in microseconds, or a negative error */
long netconfig(struct net_config *get, const struct net_config *set); /* Read the settings, then (root) replace them; either may be NULL */

/* Names (DNS). resolve() takes an address as it is, and asks the configured
 * server about anything else. Returns 0 and the address, -ENOENT if there
 * is no such name, or another negative error. */
long resolve(const char *name, uint32_t *ip);
long dns_lookup(uint32_t server, unsigned port, const char *name, uint32_t *ip); /* Ask one server (resolve() asks the configured one) */
long dns_build_query(uint8_t *buf, size_t size, unsigned id, const char *name);   /* Returns the length */
long dns_parse_reply(const uint8_t *buf, size_t len, unsigned id, uint32_t *ip); /* The first address in an answer to query `id` */
uint32_t parse_ip(const char *text);              /* "10.0.2.2" -> number; 0 if it is not an address */
void format_ip(uint32_t ip, char out[16]); /* Number -> "10.0.2.2" */

/* Terminal */
void tty_raw(int raw);                           /* 1: every key at once, no echo; 0: whole lines (the default) */
void tty_size(unsigned *columns, unsigned *rows); /* The screen's size in characters */

/* The screen as pixels (/dev/fb0): its size, and how many bytes one row of
 * it takes in the device. Four bytes per pixel: blue, green, red, unused. */
struct fb_info { unsigned width, height, pitch, bpp; };
int fb_info(struct fb_info *info); /* 0, or a negative error if there is no screen */

/* Shut the system down (root only): every process ends, the disks are left
 * clean, and the machine switches off (restart 0) or restarts (1). Returns
 * only on error (-EPERM). */
long reboot(int restart);

/* Formatted output */
void puts(const char *s); /* A string and a newline, to standard output */
void printf(const char *fmt, ...);               /* To standard output */
void fprintf(int fd, const char *fmt, ...); /* printf to a descriptor */
long readline(char *buf, size_t size);           /* From standard input; -1 at end of input */
long fdreadline(int fd, char *buf, size_t size); /* readline from a descriptor */
const char *strerror(long err); /* Words for an error number (negative or positive) */

/* Strings */
size_t strlen(const char *s); /* The C library's string functions, with their usual meanings */
int strcmp(const char *a, const char *b); /* (see strlen) */
int strncmp(const char *a, const char *b, size_t n); /* (see strlen) */
char *strcpy(char *dst, const char *src); /* (see strlen) */
char *strchr(const char *s, int c); /* The first c in s (the terminator for c == 0), or NULL */
void *memset(void *dst, int value, size_t n); /* (see strlen) */
void *memcpy(void *dst, const void *src, size_t n); /* (see strlen) */
int memcmp(const void *a, const void *b, size_t n); /* (see strlen) */
long atoi(const char *s); /* A decimal number (after an optional '-') at the start of `s` */

#endif
