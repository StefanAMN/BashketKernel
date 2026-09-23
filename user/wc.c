/* wc: count the lines, words and bytes of files or of standard input.
 *   wc [-l] [-w] [-c] [FILE...]
 * Prints, in that order, the counts asked for (all three if none is).
 * With one file or none, the counts alone (unlike POSIX, which names a
 * file given: scripts here read "N N N"); with several, each line ends
 * with the file's name, and a last line gives the totals. */
#include "ulib.h"
#include "opts.h"

static int want_lines, want_words, want_bytes;

typedef struct {
    unsigned long lines, words, bytes;
} counts_t;

/* Count what `fd` holds into *c: 0, or a negative error number */
static long count_file(int fd, counts_t *c) {
    int in_word = 0;
    char buf[512];
    long n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        c->bytes += (unsigned long)n;
        for (long i = 0; i < n; i++) {
            char ch = buf[i];
            if (ch == '\n') c->lines++;
            if (ch == ' ' || ch == '\n' || ch == '\t') {
                in_word = 0;
            } else if (!in_word) {
                in_word = 1;
                c->words++;
            }
        }
    }
    return n < 0 ? n : 0;
}

/* Print the counts asked for, then `name` if there is one */
static void print_counts(const counts_t *c, const char *name) {
    const char *gap = "";
    if (want_lines) { printf("%lu", c->lines); gap = " "; }
    if (want_words) { printf("%s%lu", gap, c->words); gap = " "; }
    if (want_bytes) printf("%s%lu", gap, c->bytes);
    if (name) printf(" %s", name);
    printf("\n");
}

int main(int argc, char **argv) {
    opts_t o;
    opts_init(&o);
    for (int c; (c = opts_next(&o, argc, argv, "lwc", "wc")) != -1;) {
        if (c == 'l') want_lines = 1;
        else if (c == 'w') want_words = 1;
        else if (c == 'c') want_bytes = 1;
        else {
            fprintf(2, "usage: wc [-l] [-w] [-c] [FILE...]\n");
            return 2;
        }
    }
    if (!want_lines && !want_words && !want_bytes) want_lines = want_words = want_bytes = 1;

    int files = argc - o.index, status = 0;
    counts_t total = { 0, 0, 0 };
    for (int i = 0; i < (files ? files : 1); i++) {
        const char *name = files ? argv[o.index + i] : NULL;
        long fd = name ? open(name, O_RDONLY) : 0;
        counts_t c = { 0, 0, 0 };
        long err = fd >= 0 ? count_file((int)fd, &c) : fd;
        if (fd > 0) close((int)fd);
        if (err < 0) {
            fprintf(2, "wc: %s: %s\n", name ? name : "standard input", strerror(err));
            status = 1;
            continue;
        }
        print_counts(&c, files > 1 ? name : NULL);
        total.lines += c.lines;
        total.words += c.words;
        total.bytes += c.bytes;
    }
    if (files > 1) print_counts(&total, "total");
    return status;
}
