/* head: the start of a file or of standard input.
 *   head [-n N | -c N] [FILE]
 * The first N lines (10 if not given), or with -c the first N bytes. */
#include "ulib.h"
#include "lines.h"
#include "opts.h"

/* Copy the first `count` bytes of `fd` to standard output: 0, or a
 * negative error number */
static long head_bytes(int fd, long count) {
    char buf[512];
    while (count > 0) {
        long want = count < (long)sizeof(buf) ? count : (long)sizeof(buf);
        long n = read(fd, buf, (size_t)want);
        if (n < 0) return n;
        if (n == 0) break;
        write(1, buf, (size_t)n);
        count -= n;
    }
    return 0;
}

/* Copy the first `count` lines of `fd` to standard output: 0, or a
 * negative error number */
static long head_lines(int fd, long count) {
    static lines_t in;
    lines_open(&in, fd);
    long len = 0;
    for (long i = 0; i < count && (len = lines_next(&in)) >= 0; i++) {
        write(1, in.line, (size_t)len);
        write(1, "\n", 1);
    }
    lines_close(&in);
    return len < -1 ? len : 0;
}

int main(int argc, char **argv) {
    long count = 10;
    int bytes = 0, bad = 0;
    opts_t o;
    opts_init(&o);
    for (int c; !bad && (c = opts_next(&o, argc, argv, "n:c:", "head")) != -1;) {
        if (c == '?' || opts_number(o.value, &count) < 0) bad = 1;
        bytes = (c == 'c');
    }
    if (bad || o.index + 1 < argc) {
        fprintf(2, "usage: head [-n N | -c N] [FILE]\n");
        return 2;
    }
    const char *name = o.index < argc ? argv[o.index] : NULL;
    long fd = name ? open(name, O_RDONLY) : 0;
    if (fd < 0) {
        fprintf(2, "head: %s: %s\n", name, strerror(fd));
        return 1;
    }
    long err = bytes ? head_bytes((int)fd, count) : head_lines((int)fd, count);
    if (err < 0) {
        fprintf(2, "head: %s\n", strerror(err));
        return 1;
    }
    return 0;
}
