/* cut: parts of every line of a file or of standard input.
 *   cut -c LIST [FILE]           the characters (bytes) at those positions
 *   cut -f LIST [-d C] [FILE]    the fields, separated by C (a tab if not given)
 * LIST is one or more of N, N-M, N- and -M, separated by commas, counting
 * from 1. What is chosen is printed in its order in the line, each part
 * once, however LIST names it. With -f the fields stay joined by C, and a
 * line without any C is printed whole. Lines may be of any length
 * (user/lib/lines.h). */
#include "ulib.h"
#include "lines.h"

#define MAX_RANGES 32
#define NO_END     0x7FFFFFFFFFFFFFFFL

static struct { long from, to; } ranges[MAX_RANGES];   /* Inclusive, from 1 */
static int range_count;

/* Read a decimal number of at least one digit at *p, moving *p past it.
 * Returns it, or -1 if there is no digit or it is 0. */
static long number(const char **p) {
    long value = 0;
    int digits = 0;
    while (**p >= '0' && **p <= '9' && digits < 18) {
        value = value * 10 + (*(*p)++ - '0');
        digits++;
    }
    return digits && value > 0 ? value : -1;
}

/* Parse LIST into `ranges`. 0, or -1 if it is malformed or too long. */
static int parse_list(const char *p) {
    for (;;) {
        if (range_count >= MAX_RANGES) return -1;
        long from = 1, to;
        if (*p == '-') {                      /* "-M" */
            p++;
            if ((to = number(&p)) < 0) return -1;
        } else {
            if ((from = number(&p)) < 0) return -1;
            to = from;
            if (*p == '-') {                  /* "N-" or "N-M" */
                p++;
                to = (*p >= '0' && *p <= '9') ? number(&p) : NO_END;
                if (to < from) return -1;
            }
        }
        ranges[range_count].from = from;
        ranges[range_count].to = to;
        range_count++;
        if (*p == '\0') return 0;
        if (*p++ != ',') return -1;
    }
}

/* Whether position `n` (from 1) is in LIST */
static int chosen(long n) {
    for (int i = 0; i < range_count; i++) {
        if (n >= ranges[i].from && n <= ranges[i].to) return 1;
    }
    return 0;
}

/* Print the chosen bytes of `line` (`len` long), and a newline */
static void cut_chars(const char *line, long len) {
    /* Runs of chosen bytes go out in one write each */
    long start = -1;
    for (long i = 0; i <= len; i++) {
        int take = i < len && chosen(i + 1);
        if (take && start < 0) start = i;
        if (!take && start >= 0) {
            write(1, line + start, (size_t)(i - start));
            start = -1;
        }
    }
    write(1, "\n", 1);
}

/* Print the chosen fields of `line` (`len` long), separated by `delim`,
 * and a newline; a line without `delim` is printed whole */
static void cut_fields(const char *line, long len, char delim) {
    long at = 0;
    while (at < len && line[at] != delim) at++;
    if (at == len) {
        write(1, line, (size_t)len);
        write(1, "\n", 1);
        return;
    }
    long field = 1, start = 0;
    int printed = 0;
    for (long i = 0; i <= len; i++) {
        if (i < len && line[i] != delim) continue;
        if (chosen(field)) {
            if (printed) write(1, &delim, 1);
            write(1, line + start, (size_t)(i - start));
            printed = 1;
        }
        field++;
        start = i + 1;
    }
    write(1, "\n", 1);
}

int main(int argc, char **argv) {
    int by_fields = -1;      /* -1: neither -c nor -f yet */
    char delim = '\t';
    int delim_given = 0;
    const char *list = NULL;
    int arg = 1;
    for (; arg + 1 < argc && argv[arg][0] == '-'; arg += 2) {
        if (strcmp(argv[arg], "-c") == 0 || strcmp(argv[arg], "-f") == 0) {
            if (list) by_fields = -2;          /* Both: not allowed */
            else by_fields = argv[arg][1] == 'f';
            list = argv[arg + 1];
        } else if (strcmp(argv[arg], "-d") == 0 && strlen(argv[arg + 1]) == 1) {
            delim = argv[arg + 1][0];
            delim_given = 1;
        } else {
            break;
        }
    }
    if (by_fields < 0 || (delim_given && !by_fields) || arg + 1 < argc ||
        (arg < argc && argv[arg][0] == '-') || parse_list(list) < 0) {
        fprintf(2, "usage: cut -c LIST [FILE]  or  cut -f LIST [-d C] [FILE]\n"
                   "       (LIST: N, N-M, N- or -M, separated by commas, from 1)\n");
        return 2;
    }
    long fd = arg < argc ? open(argv[arg], O_RDONLY) : 0;
    if (fd < 0) {
        fprintf(2, "cut: %s: %s\n", argv[arg], strerror(fd));
        return 1;
    }

    static lines_t in;
    lines_open(&in, (int)fd);
    long len;
    while ((len = lines_next(&in)) >= 0) {
        if (by_fields) cut_fields(in.line, len, delim);
        else cut_chars(in.line, len);
    }
    lines_close(&in);
    if (len < -1) {
        fprintf(2, "cut: %s\n", strerror(len));
        return 1;
    }
    return 0;
}
