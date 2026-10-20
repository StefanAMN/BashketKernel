/* tail: the last lines of a file or of standard input.
 *   tail [-n N] [FILE]      the last N lines (10 if not given; at most 10000)
 * The input is read once; the last N lines are kept as it goes. */
#include "ulib.h"
#include "lines.h"

#define TAIL_MAX 10000

int main(int argc, char **argv) {
    long count = 10;
    int arg = 1;
    if (arg + 1 < argc && strcmp(argv[arg], "-n") == 0) {
        count = atoi(argv[arg + 1]);
        arg += 2;
    }
    if (count < 0 || count > TAIL_MAX || arg + 1 < argc) {
        fprintf(2, "usage: tail [-n N] [FILE]   (N up to %d)\n", TAIL_MAX);
        return 2;
    }
    long fd = arg < argc ? open(argv[arg], O_RDONLY) : 0;
    if (fd < 0) {
        fprintf(2, "tail: %s: %s\n", argv[arg], strerror(fd));
        return 1;
    }

    /* A ring of the last `count` lines: kept[(first + i) % count] is the
     * i-th oldest, each a copy with its length */
    char **kept = count ? malloc(sizeof(char *) * (size_t)count) : NULL;
    long *lengths = count ? malloc(sizeof(long) * (size_t)count) : NULL;
    if (count && (!kept || !lengths)) {
        fprintf(2, "tail: out of memory\n");
        return 1;
    }
    long held = 0, first = 0, len;
    static lines_t in;
    lines_open(&in, (int)fd);
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
    if (len < -1) {
        fprintf(2, "tail: %s\n", strerror(len));
        return 1;
    }
    for (long i = 0; i < held; i++) {
        long slot = (first + i) % count;
        write(1, kept[slot], (size_t)lengths[slot]);
        write(1, "\n", 1);
    }
    return 0;
}
