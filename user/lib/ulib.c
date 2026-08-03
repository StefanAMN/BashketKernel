/* A very small C library for BashketKernel user programs. */
#include "ulib.h"

/* --- Process ------------------------------------------------------------- */

void exit(int status) {
    syscall1(SYS_EXIT, status);
    for (;;) { }
}

long fork(void)                                    { return syscall0(SYS_FORK); }
long execve(const char *path, char *const argv[], char *const envp[]) { return syscall3(SYS_EXECVE, path, argv, envp); }
long execv(const char *path, char *const argv[])   { return execve(path, argv, environ); }
long waitpid(long pid, int *status)                { return syscall2(SYS_WAITPID, pid, status); }
long getpid(void)                                  { return syscall0(SYS_GETPID); }
long getppid(void)                                 { return syscall0(SYS_GETPPID); }
long kill(long pid, int sig)                       { return syscall2(SYS_KILL, pid, sig); }
void yield(void)                                   { syscall0(SYS_YIELD); }
void sleep_ms(unsigned long ms)                    { syscall1(SYS_SLEEP, ms); }
long uptime_ticks(void)                            { return syscall0(SYS_TIME); }
unsigned long clock_ns(void)                       { return (unsigned long)syscall0(SYS_CLOCK_NS); }
long sleep_ns(unsigned long nanoseconds)           { return syscall1(SYS_NANOSLEEP, nanoseconds); }
long set_priority(int priority)                    { return syscall1(SYS_SETPRIORITY, priority); }
void ignore_interrupt(int ignore)                  { signal(SIGINT, ignore ? SIG_IGN : SIG_DFL); }

/* In crt0.S: where a signal handler returns to. It asks the kernel to put
 * back whatever the signal interrupted. */
extern void __sigreturn(void);

sighandler_t signal(int sig, sighandler_t handler) {
    long previous = syscall3(SYS_SIGNAL, sig, handler, __sigreturn);
    return previous < 0 ? SIG_ERR : (sighandler_t)previous;
}

/* --- Users and permissions ------------------------------------------------- */

long getuid(void)                                  { return syscall0(SYS_GETUID); }
long setuid(unsigned uid)                          { return syscall1(SYS_SETUID, uid); }
long chmod(const char *path, unsigned mode)        { return syscall2(SYS_CHMOD, path, mode); }
long chown(const char *path, unsigned uid)         { return syscall2(SYS_CHOWN, path, uid); }

/* --- Environment ---------------------------------------------------------- */

/* Set by crt0 to the vector the kernel put on our stack. The environment is
 * ordinary memory of this process: the kernel does not know what is in it,
 * it only copies the strings when we exec. */
char **environ;

const char *getenv(const char *name) {
    size_t len = strlen(name);
    for (char **e = environ; e && *e; e++) {
        if (strncmp(*e, name, len) == 0 && (*e)[len] == '=') return *e + len + 1;
    }
    return NULL;
}

int setenv(const char *name, const char *value) {
    size_t name_len = strlen(name), value_len = strlen(value);
    char *entry = malloc(name_len + value_len + 2);
    if (!entry) return -1;
    memcpy(entry, name, name_len);
    entry[name_len] = '=';
    memcpy(entry + name_len + 1, value, value_len + 1);

    /* Replace an existing entry in place */
    size_t count = 0;
    for (char **e = environ; e && *e; e++, count++) {
        if (strncmp(*e, name, name_len) == 0 && (*e)[name_len] == '=') {
            *e = entry;
            return 0;
        }
    }

    /* Otherwise the vector has to grow: copy it with room for one more.
     * (The old vector may be the one on the stack, so it is never freed.) */
    char **grown = malloc((count + 2) * sizeof(char *));
    if (!grown) return -1;
    for (size_t i = 0; i < count; i++) grown[i] = environ[i];
    grown[count] = entry;
    grown[count + 1] = NULL;
    environ = grown;
    return 0;
}

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

void *mmap_shared(size_t length) {
    long ret = syscall4(SYS_MMAP, 0, length, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS);
    return ret < 0 ? NULL : (void *)ret;
}

long munmap(void *addr, size_t length) { return syscall2(SYS_MUNMAP, addr, length); }

long shmget(unsigned key, size_t size)  { return syscall2(SYS_SHMGET, key, size); }
long shmrm(long id)                     { return syscall1(SYS_SHMRM, id); }

void *shmat(long id) {
    long ret = syscall1(SYS_SHMAT, id);
    return ret < 0 ? NULL : (void *)ret;
}

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
long chdir(const char *path)                       { return syscall1(SYS_CHDIR, path); }
long getcwd(char *buf, size_t size)                { return syscall2(SYS_GETCWD, buf, size); }
long unlink(const char *path)                      { return syscall1(SYS_UNLINK, path); }

/* --- Network ---------------------------------------------------------------- */

long socket(void)                                   { return syscall0(SYS_SOCKET); }
long bind(int fd, unsigned port)                    { return syscall2(SYS_BIND, fd, port); }
long ping(uint32_t ip, unsigned long timeout_ms)    { return syscall2(SYS_PING, ip, timeout_ms); }

long sendto(int fd, const void *buf, size_t len, uint32_t ip, unsigned port) {
    return syscall5(SYS_SENDTO, fd, buf, len, ip, port);
}

long recvfrom(int fd, void *buf, size_t len, struct net_address *from, unsigned long timeout_ms) {
    return syscall5(SYS_RECVFROM, fd, buf, len, from, timeout_ms);
}

uint32_t parse_ip(const char *text) {
    uint32_t ip = 0;
    for (int part = 0; part < 4; part++) {
        if (*text < '0' || *text > '9') return 0;
        unsigned value = 0;
        int digits = 0;
        while (*text >= '0' && *text <= '9') {
            value = value * 10 + (unsigned)(*text++ - '0');
            if (++digits > 3 || value > 255) return 0;
        }
        ip = (ip << 8) | value;
        if (part < 3) {
            if (*text != '.') return 0;
            text++;
        }
    }
    return *text == '\0' ? ip : 0;
}

void format_ip(uint32_t ip, char out[16]) {
    size_t len = 0;
    for (int shift = 24; shift >= 0; shift -= 8) {
        unsigned part = (ip >> shift) & 0xFF;
        if (part >= 100) out[len++] = (char)('0' + part / 100);
        if (part >= 10) out[len++] = (char)('0' + part / 10 % 10);
        out[len++] = (char)('0' + part % 10);
        if (shift > 0) out[len++] = '.';
    }
    out[len] = '\0';
}

void tty_raw(int raw) { syscall1(SYS_TTY_RAW, raw); }

void tty_size(unsigned *columns, unsigned *rows) {
    unsigned size[2] = { 80, 25 };
    syscall1(SYS_TTY_SIZE, size);
    if (columns) *columns = size[0];
    if (rows) *rows = size[1];
}

const char *strerror(long err) {
    switch (err < 0 ? -err : err) {
        case 0:         return "success";
        case EPERM:     return "operation not permitted";
        case EACCES:    return "permission denied";
        case ENOENT:    return "no such file or directory";
        case ESRCH:     return "no such process";
        case EINTR:     return "interrupted by a signal";
        case EIO:       return "input/output error";
        case ENODEV:    return "no such device";
        case E2BIG:     return "argument list too long";
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
        case EROFS:     return "read-only filesystem";
        case EPIPE:     return "broken pipe";
        case ENOSYS:    return "not implemented";
        case EMSGSIZE:  return "message too long";
        case EADDRINUSE: return "port already in use";
        case ENETDOWN:  return "no network";
        case ETIMEDOUT: return "timed out";
        case EHOSTUNREACH: return "host unreachable";
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

static void out_number(outbuf_t *out, unsigned long value, unsigned base, int negative, int width, int zero_pad) {
    char digits[24];
    int n = 0;
    do {
        unsigned d = (unsigned)(value % base);
        digits[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        value /= base;
    } while (value);

    int padding = width - n - (negative ? 1 : 0);
    if (!zero_pad) while (padding-- > 0) out_char(out, ' ');
    if (negative) out_char(out, '-');
    if (zero_pad) while (padding-- > 0) out_char(out, '0');
    while (n) out_char(out, digits[--n]);
}

/* Supports %d %u %x %p %s %c %%, the l length modifier, and a field width
 * with optional zero padding (%5d, %02d, %8s). Fields are right-aligned. */
static void format(int fd, const char *fmt, va_list args) {
    outbuf_t out;
    out.fd = fd;
    out.len = 0;

    for (; *fmt; fmt++) {
        if (*fmt != '%') { out_char(&out, *fmt); continue; }

        fmt++;
        int zero_pad = (*fmt == '0');
        if (zero_pad) fmt++;
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            if (width < 64) width = width * 10 + (*fmt - '0');
            fmt++;
        }
        int is_long = 0;
        if (*fmt == 'l') { is_long = 1; fmt++; }

        switch (*fmt) {
            case 'd': {
                long v = is_long ? va_arg(args, long) : va_arg(args, int);
                out_number(&out, v < 0 ? -(unsigned long)v : (unsigned long)v, 10, v < 0, width, zero_pad);
                break;
            }
            case 'u': out_number(&out, is_long ? va_arg(args, unsigned long) : va_arg(args, unsigned), 10, 0, width, zero_pad); break;
            case 'x': out_number(&out, is_long ? va_arg(args, unsigned long) : va_arg(args, unsigned), 16, 0, width, zero_pad); break;
            case 'p': out_string(&out, "0x"); out_number(&out, (unsigned long)va_arg(args, void *), 16, 0, 0, 0); break;
            case 's': {
                const char *str = va_arg(args, const char *);
                if (!str) str = "(null)";
                for (int pad = width - (int)strlen(str); pad > 0; pad--) out_char(&out, ' ');
                out_string(&out, str);
                break;
            }
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

/* Read one line from standard input into buf, without the newline. Works
 * the same for the terminal, a file or a pipe; echo and line editing are the
 * terminal's job, not ours. Returns the length, or -1 at end of input with
 * nothing read. A line longer than the buffer is cut. */
long readline(char *buf, size_t size) {
    return fdreadline(0, buf, size);
}

/* The same, from any descriptor */
long fdreadline(int fd, char *buf, size_t size) {
    size_t len = 0;
    for (;;) {
        char c;
        long n = read(fd, &c, 1);
        if (n == -EINTR) continue;  /* A signal handler ran: carry on reading */
        if (n <= 0) {
            buf[len] = '\0';
            return len > 0 ? (long)len : -1;
        }
        if (c == '\n') {
            buf[len] = '\0';
            return (long)len;
        }
        if (len < size - 1) buf[len++] = c;
    }
}
