/* Reading a file line by line (see lines.h) */
#include "lines.h"

void lines_open(lines_t *r, int fd) {
    r->fd = fd;
    r->pos = r->len = 0;
    r->line = NULL;
    r->cap = 0;
    r->ended = 0;
}

/* Make room for `need` bytes in the line buffer. 0, or -ENOMEM. */
static long grow(lines_t *r, size_t need) {
    if (need <= r->cap) return 0;
    if (need > LINES_MAX_LINE + 1) return -ENOMEM;
    size_t cap = r->cap ? r->cap : 128;
    while (cap < need) cap *= 2;
    char *bigger = malloc(cap);
    if (!bigger) return -ENOMEM;
    if (r->line) {
        memcpy(bigger, r->line, r->cap);
        free(r->line);
    }
    r->line = bigger;
    r->cap = cap;
    return 0;
}

long lines_next(lines_t *r) {
    size_t n = 0;
    long err = grow(r, 1);
    if (err < 0) return err;
    for (;;) {
        if (r->pos == r->len) {                        /* Refill */
            if (r->ended) break;
            long got;
            while ((got = read(r->fd, r->buf, sizeof(r->buf))) == -EINTR) { }
            if (got < 0) return got;
            if (got == 0) { r->ended = 1; break; }
            r->pos = 0;
            r->len = (size_t)got;
        }
        char c = r->buf[r->pos++];
        if (c == '\n') {
            r->line[n] = '\0';
            return (long)n;
        }
        if ((err = grow(r, n + 2)) < 0) return err;
        r->line[n++] = c;
    }
    r->line[n] = '\0';
    return n > 0 ? (long)n : -1;                       /* A last line without '\n', or the end */
}

void lines_close(lines_t *r) {
    free(r->line);
    r->line = NULL;
    r->cap = 0;
}
