/* tail: the end of a file or of standard input.
 *   tail [-n N | -c N] [FILE]
 * The last N lines (10 if not given; at most 10000), or with -c the last
 * N bytes (at most 1 MiB). The input is read once; what may be the end is
 * kept as it goes. */
#include "ulib.h"
#include "lines.h"
#include "opts.h"

#define TAIL_MAX       10000
#define TAIL_MAX_BYTES (1024 * 1024)

/* Print the last `count` bytes of `fd`, kept in a ring as it is read: 0,
 * or a negative error number */
static long tail_bytes(int fd, long count) {
    char *ring = count ? malloc((size_t)count) : NULL;
    if (count && !ring) return -ENOMEM;
    long total = 0;                                    /* Bytes read; the ring holds the last of them */
    char buf[512];
    long n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (long i = 0; i < n && count; i++) ring[(total + i) % count] = buf[i];
        total += n;
    }
    if (n < 0) return n;
    long held = total < count ? total : count;
    long first = total - held;                         /* The oldest byte kept */
    if (held > 0) {                                    /* At most two pieces: to the ring's end, then from its start */
        long start = first % count, piece = held < count - start ? held : count - start;
        write(1, ring + start, (size_t)piece);
        if (held > piece) write(1, ring, (size_t)(held - piece));
    }
    free(ring);
    return 0;
}

/* Print the last `count` lines of `fd`: 0, or a negative error number */
static long tail_lines(int fd, long count) {
    /* A ring of the last `count` lines: kept[(first + i) % count] is the
     * i-th oldest, each a copy with its length */
    char **kept = count ? malloc(sizeof(char *) * (size_t)count) : NULL;
    long *lengths = count ? malloc(sizeof(long) * (size_t)count) : NULL;
    if (count && (!kept || !lengths)) return -ENOMEM;
    long held = 0, first = 0, len;
    static lines_t in;
    lines_open(&in, fd);
    while ((len = lines_next(&in)) >= 0) {
        if (count == 0) continue;
        char *copy = malloc((size_t)len + 1);
        if (!copy) { len = -ENOMEM; break; }
        memcpy(copy, in.line, (size_t)len + 1);
        long slot;
        if (held < count) {
            slot = (first + held++) % count;
        } else {                                       /* Full: the oldest goes */
            slot = first;
            free(kept[slot]);
            first = (first + 1) % count;
        }
        kept[slot] = copy;
        lengths[slot] = len;
    }
    lines_close(&in);
    if (len < -1) return len;
    for (long i = 0; i < held; i++) {
        long slot = (first + i) % count;
        write(1, kept[slot], (size_t)lengths[slot]);
        write(1, "\n", 1);
    }
    return 0;
}

int main(int argc, char **argv) {
    long count = 10;
    int bytes = 0, bad = 0;
    opts_t o;
    opts_init(&o);
    for (int c; !bad && (c = opts_next(&o, argc, argv, "n:c:", "tail")) != -1;) {
        if (c == '?' || opts_number(o.value, &count) < 0) bad = 1;
        bytes = (c == 'c');
    }
    if (bad || count > (bytes ? TAIL_MAX_BYTES : TAIL_MAX) || o.index + 1 < argc) {
        fprintf(2, "usage: tail [-n N | -c N] [FILE]   (N up to %d lines or %d bytes)\n", TAIL_MAX, TAIL_MAX_BYTES);
        return 2;
    }
    const char *name = o.index < argc ? argv[o.index] : NULL;
    long fd = name ? open(name, O_RDONLY) : 0;
    if (fd < 0) {
        fprintf(2, "tail: %s: %s\n", name, strerror(fd));
        return 1;
    }
    long err = bytes ? tail_bytes((int)fd, count) : tail_lines((int)fd, count);
    if (err < 0) {
        fprintf(2, "tail: %s\n", err == -ENOMEM ? "out of memory" : strerror(err));
        return 1;
    }
    return 0;
}
