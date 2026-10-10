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
long execve(const char *path, char *const argv[], char *const envp[]);
long execv(const char *path, char *const argv[]);    /* Passes on our own environment */

/* Environment: "NAME=value" strings, inherited across fork and exec */
extern char **environ;
const char *getenv(const char *name);
int setenv(const char *name, const char *value);     /* 0, or -1 if out of memory */
long waitpid(long pid, int *status, int options);  /* pid -1: any child. options: 0, or WNOHANG */
long getpid(void);
long getppid(void);
long kill(long pid, int sig);
void yield(void);
void sleep_ms(unsigned long ms);
long uptime_ticks(void);                     /* Timer ticks (10 ms each) since boot */
unsigned long clock_ns(void);                /* Nanoseconds since boot */
long sleep_ns(unsigned long nanoseconds);    /* Sleeps to within microseconds; -EINTR if a signal cut it short */
long set_priority(int priority);    /* 0 high, 1 normal, 2 low */
void ignore_interrupt(int ignore);   /* 1: Ctrl-C does not end this process */

/* Signals */
#define SIGINT   2
#define SIGKILL  9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGTERM 15
#define SIGCHLD 17                  /* A child has exited; ignored unless a handler is set */
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
long sigaction(int sig, const struct sigaction *action, struct sigaction *old);

/* Hold signals back: a blocked signal waits until it is unblocked. SIGKILL
 * can not be blocked. Either pointer may be NULL (old alone reads the set). */
#define SIG_BLOCK   0               /* Add `set` to the blocked signals */
#define SIG_UNBLOCK 1               /* Remove it */
#define SIG_SETMASK 2               /* Replace them with it */
long sigprocmask(int how, const uint32_t *set, uint32_t *old);

/* Users and permissions. uid 0 is root and may do anything. */
long getuid(void);
long setuid(unsigned uid);                   /* Root may become anyone; nobody else may change */
long chmod(const char *path, unsigned mode); /* For example 0644: owner reads and writes, others read */
long chown(const char *path, unsigned uid);  /* Root only */

/* Memory */
void *sbrk(long increment);
void *mmap_anon(size_t length);
void *mmap_shared(size_t length);            /* Stays shared with children created by fork */

/* Shared memory between any processes that agree on a key */
long shmget(unsigned key, size_t size);      /* Find or create; returns an id */
void *shmat(long id);                        /* Map it here; NULL on failure. munmap() to detach. */
long shmrm(long id);                         /* Release the key; memory goes with the last mapping */
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
long dup(int fd);                            /* The same open file under the lowest free descriptor */
long pipe2(int fds[2], int flags);           /* flags: 0 or O_CLOEXEC */
long fcntl(int fd, int command, long arg);   /* F_GETFD, or F_SETFD with FD_CLOEXEC or 0 */
long readdir(int fd, struct dirent *entry);   /* 1 = entry filled, 0 = end */
long mkdir(const char *path);
long chdir(const char *path);
long getcwd(char *buf, size_t size);          /* Length of the path, or a negative error */
long unlink(const char *path);

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
long sendto(int fd, const void *buf, size_t len, uint32_t ip, unsigned port);
long recvfrom(int fd, void *buf, size_t len, struct net_address *from, unsigned long timeout_ms); /* 0: wait for ever */
long ping(uint32_t ip, unsigned long timeout_ms); /* Round-trip time in microseconds, or a negative error */
long netconfig(struct net_config *get, const struct net_config *set); /* Read the settings, then (root) replace them; either may be NULL */

/* Names (DNS). resolve() takes an address as it is, and asks the configured
 * server about anything else. Returns 0 and the address, -ENOENT if there
 * is no such name, or another negative error. */
long resolve(const char *name, uint32_t *ip);
long dns_lookup(uint32_t server, unsigned port, const char *name, uint32_t *ip);
long dns_build_query(uint8_t *buf, size_t size, unsigned id, const char *name);   /* Returns the length */
long dns_parse_reply(const uint8_t *buf, size_t len, unsigned id, uint32_t *ip);
uint32_t parse_ip(const char *text);              /* "10.0.2.2" -> number; 0 if it is not an address */
void format_ip(uint32_t ip, char out[16]);

/* Terminal */
void tty_raw(int raw);                           /* 1: every key at once, no echo; 0: whole lines (the default) */
void tty_size(unsigned *columns, unsigned *rows);

/* Formatted output */
void puts(const char *s);
void printf(const char *fmt, ...);               /* To standard output */
void fprintf(int fd, const char *fmt, ...);
long readline(char *buf, size_t size);           /* From standard input; -1 at end of input */
long fdreadline(int fd, char *buf, size_t size);
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
