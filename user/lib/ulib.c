/* A very small C library for BashketKernel user programs. */
#include "ulib.h"

/* --- Process ------------------------------------------------------------- */

void exit(int status) {
    syscall1(SYS_EXIT, status);
    for (;;) { }
}

long fork(void)                                    { return syscall0(SYS_FORK); }
long execve(const char *path, char *const argv[])  { return syscall2(SYS_EXECVE, path, argv); }
long waitpid(long pid, int *status)                { return syscall2(SYS_WAITPID, pid, status); }
long getpid(void)                                  { return syscall0(SYS_GETPID); }
long getppid(void)                                 { return syscall0(SYS_GETPPID); }
long kill(long pid, int sig)                       { return syscall2(SYS_KILL, pid, sig); }
void yield(void)                                   { syscall0(SYS_YIELD); }
void sleep_ms(unsigned long ms)                    { syscall1(SYS_SLEEP, ms); }
long uptime_ticks(void)                            { return syscall0(SYS_TIME); }

/* --- Memory -------------------------------------------------------------- */

void *sbrk(long increment) {
    long old = syscall1(SYS_BRK, 0);
    if (increment == 0) return (void *)old;

    long now = syscall1(SYS_BRK, old + increment);
    if (now != old + increment) return (void *)-1; /* The kernel refused */
    return (void *)old;
}

void *mmap_anon(size_t length) {
    long ret = syscall4(SYS_MMAP, 0, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
    return ret < 0 ? NULL : (void *)ret;
}

long munmap(void *addr, size_t length) { return syscall2(SYS_MUNMAP, addr, length); }

/* First-fit free list on top of sbrk. Each block has a header; freed blocks
 * are reused but never returned to the kernel or merged. Enough for small
 * programs, and short enough to read in one sitting. */
typedef struct block {
    size_t size;
    struct block *next_free;
} block_t;

static block_t *free_list = NULL;

void *malloc(size_t size) {
    if (size == 0) return NULL;
    size = (size + 15) & ~(size_t)15;

    for (block_t **link = &free_list; *link; link = &(*link)->next_free) {
        if ((*link)->size >= size) {
            block_t *block = *link;
            *link = block->next_free;
            return block + 1;
        }
    }

    block_t *block = sbrk((long)(sizeof(block_t) + size));
    if (block == (void *)-1) return NULL;
    block->size = size;
    return block + 1;
}

void free(void *ptr) {
    if (!ptr) return;
    block_t *block = (block_t *)ptr - 1;
    block->next_free = free_list;
    free_list = block;
}

/* --- Strings ------------------------------------------------------------- */

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    for (; n > 0; n--, a++, b++) {
        if (*a != *b || !*a) return (unsigned char)*a - (unsigned char)*b;
    }
    return 0;
}

char *strcpy(char *dst, const char *src) {
    char *out = dst;
    while ((*dst++ = *src++)) { }
    return out;
}

char *strchr(const char *s, int c) {
    for (; *s; s++) {
        if (*s == (char)c) return (char *)s;
    }
    return NULL;
}

void *memset(void *dst, int value, size_t n) {
    unsigned char *d = dst;
    while (n--) *d++ = (unsigned char)value;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b;
    for (; n > 0; n--, x++, y++) {
        if (*x != *y) return *x - *y;
    }
    return 0;
}

long atoi(const char *s) {
    long value = 0;
    int negative = 0;
    if (*s == '-') { negative = 1; s++; }
    while (*s >= '0' && *s <= '9') value = value * 10 + (*s++ - '0');
    return negative ? -value : value;
}

/* --- Files --------------------------------------------------------------- */

long open(const char *path, int flags)             { return syscall2(SYS_OPEN, path, flags); }
long close(int fd)                                 { return syscall1(SYS_CLOSE, fd); }
long read(int fd, void *buf, size_t count)         { return syscall3(SYS_READ, fd, buf, count); }
long write(int fd, const void *buf, size_t count)  { return syscall3(SYS_WRITE, fd, buf, count); }
long lseek(int fd, long offset, int whence)        { return syscall3(SYS_LSEEK, fd, offset, whence); }
long pipe(int fds[2])                              { return syscall1(SYS_PIPE, fds); }
long dup2(int old_fd, int new_fd)                  { return syscall2(SYS_DUP2, old_fd, new_fd); }
long readdir(int fd, struct dirent *entry)         { return syscall2(SYS_READDIR, fd, entry); }
long mkdir(const char *path)                       { return syscall1(SYS_MKDIR, path); }
long unlink(const char *path)                      { return syscall1(SYS_UNLINK, path); }

const char *strerror(long err) {
    switch (err < 0 ? -err : err) {
        case 0:         return "success";
        case ENOENT:    return "no such file or directory";
        case ESRCH:     return "no such process";
        case EBADF:     return "bad file descriptor";
        case ECHILD:    return "no child processes";
        case ENOMEM:    return "out of memory";
        case EFAULT:    return "bad address";
        case EBUSY:     return "file is in use";
        case EEXIST:    return "file exists";
        case ENOTDIR:   return "not a directory";
        case EISDIR:    return "is a directory";
        case EINVAL:    return "invalid argument";
        case EMFILE:    return "too many open files";
        case ENOSPC:    return "no space left";
        case ESPIPE:    return "cannot seek";
        case EPIPE:     return "broken pipe";
        case ENOSYS:    return "not implemented";
        case ENOTEMPTY: return "directory not empty";
        default:        return "unknown error";
    }
}

/* --- Formatted output ---------------------------------------------------- */

void puts(const char *s) {
    printf("%s\n", s);
}

/* Output is collected in a buffer and written with one system call per
 * printf, so lines from two processes do not interleave mid-line. */
typedef struct {
    int fd;
    char data[256];
    size_t len;
} outbuf_t;

static void out_flush(outbuf_t *out) {
    size_t sent = 0;
    while (sent < out->len) {
        long n = write(out->fd, out->data + sent, out->len - sent);
        if (n <= 0) break; /* Closed pipe, bad descriptor: drop the rest */
        sent += (size_t)n;
    }
    out->len = 0;
}

static void out_char(outbuf_t *out, char c) {
    if (out->len == sizeof(out->data)) out_flush(out);
    out->data[out->len++] = c;
}

static void out_string(outbuf_t *out, const char *s) {
    while (*s) out_char(out, *s++);
}

static void out_number(outbuf_t *out, unsigned long value, unsigned base, int negative) {
    char digits[24];
    int n = 0;
    do {
        unsigned d = (unsigned)(value % base);
        digits[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        value /= base;
    } while (value);
    if (negative) out_char(out, '-');
    while (n) out_char(out, digits[--n]);
}

/* Supports %d %u %x %p %s %c %% and the l length modifier. */
static void format(int fd, const char *fmt, va_list args) {
    outbuf_t out;
    out.fd = fd;
    out.len = 0;

    for (; *fmt; fmt++) {
        if (*fmt != '%') { out_char(&out, *fmt); continue; }

        fmt++;
        int is_long = 0;
        if (*fmt == 'l') { is_long = 1; fmt++; }

        switch (*fmt) {
            case 'd': {
                long v = is_long ? va_arg(args, long) : va_arg(args, int);
                out_number(&out, v < 0 ? -(unsigned long)v : (unsigned long)v, 10, v < 0);
                break;
            }
            case 'u': out_number(&out, is_long ? va_arg(args, unsigned long) : va_arg(args, unsigned), 10, 0); break;
            case 'x': out_number(&out, is_long ? va_arg(args, unsigned long) : va_arg(args, unsigned), 16, 0); break;
            case 'p': out_string(&out, "0x"); out_number(&out, (unsigned long)va_arg(args, void *), 16, 0); break;
            case 's': { const char *s = va_arg(args, const char *); out_string(&out, s ? s : "(null)"); break; }
            case 'c': out_char(&out, (char)va_arg(args, int)); break;
            case '%': out_char(&out, '%'); break;
            case '\0': fmt--; break;
            default:  out_char(&out, '%'); out_char(&out, *fmt); break;
        }
    }

    out_flush(&out);
}

void printf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    format(1, fmt, args);
    va_end(args);
}

void fprintf(int fd, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    format(fd, fmt, args);
    va_end(args);
}

/* Read one line from standard input into buf, without the newline. When
 * input is the console this echoes what is typed and handles backspace.
 * Returns the length, or -1 at end of input with nothing read. */
long readline(char *buf, size_t size) {
    size_t len = 0;
    for (;;) {
        char c;
        long n = read(0, &c, 1);
        if (n <= 0) {
            buf[len] = '\0';
            return len > 0 ? (long)len : -1;
        }

        if (c == '\n' || c == '\r') {
            write(1, "\n", 1);
            buf[len] = '\0';
            return (long)len;
        }
        if (c == '\b' || c == 127) {
            if (len > 0) { len--; write(1, "\b \b", 3); }
            continue;
        }
        if (len < size - 1) {
            buf[len++] = c;
            write(1, &c, 1);
        }
    }
}
