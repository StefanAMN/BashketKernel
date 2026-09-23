/* uniq: lines of a file or of standard input, with repeats in a row shown
 * once.
 *   uniq [-c] [-d] [-i] [FILE]
 * Only neighbouring lines are compared, so a sorted input ("sort | uniq")
 * shows every different line once. -c puts how many times each line came
 * in a row before it ("3 text"); -d shows only lines that came more than
 * once; -i compares lines ignoring the case of ASCII letters (the first of
 * a run is the one shown). Lines may be of any length (user/lib/lines.h). */
#include "ulib.h"
#include "lines.h"
#include "opts.h"

static int show_count = 0;
static int only_repeated = 0;
static int ignore_case = 0;

/* Whether two lines of `len` bytes are the same, as -i asks */
static int same_line(const char *a, const char *b, long len) {
    if (!ignore_case) return memcmp(a, b, (size_t)len) == 0;
    for (long i = 0; i < len; i++) {
        char x = (a[i] >= 'A' && a[i] <= 'Z') ? (char)(a[i] + 32) : a[i];
        char y = (b[i] >= 'A' && b[i] <= 'Z') ? (char)(b[i] + 32) : b[i];
        if (x != y) return 0;
    }
    return 1;
}

/* Write one output line: `line` (`len` bytes) seen `times` in a row */
static void emit(const char *line, long len, long times) {
    if (only_repeated && times < 2) return;
    if (show_count) printf("%ld ", times);
    write(1, line, (size_t)len);
    write(1, "\n", 1);
}

int main(int argc, char **argv) {
    opts_t o;
    opts_init(&o);
    int c = 0;
    while ((c = opts_next(&o, argc, argv, "cdi", "uniq")) != -1 && c != '?') {
        if (c == 'c') show_count = 1;
        else if (c == 'd') only_repeated = 1;
        else ignore_case = 1;
    }
    int arg = o.index;
    if (c == '?' || arg + 1 < argc) {
        fprintf(2, "usage: uniq [-c] [-d] [-i] [FILE]\n");
        return 2;
    }
    long fd = arg < argc ? open(argv[arg], O_RDONLY) : 0;
    if (fd < 0) {
        fprintf(2, "uniq: %s: %s\n", argv[arg], strerror(fd));
        return 1;
    }

    /* The line being counted is a copy: the reader's buffer is reused */
    char *kept = NULL;
    long kept_len = 0, times = 0;
    static lines_t in;
    lines_open(&in, (int)fd);
    long len;
    int status = 0;
    while ((len = lines_next(&in)) >= 0) {
        if (times > 0 && len == kept_len && same_line(in.line, kept, len)) {
            times++;
            continue;
        }
        if (times > 0) emit(kept, kept_len, times);
        free(kept);
        kept = malloc((size_t)len + 1);
        if (!kept) {
            fprintf(2, "uniq: out of memory\n");
            status = 1;
            times = 0;
            break;
        }
        memcpy(kept, in.line, (size_t)len);
        kept_len = len;
        times = 1;
    }
    if (times > 0) emit(kept, kept_len, times);
    free(kept);
    lines_close(&in);
    if (len < -1) {
        fprintf(2, "uniq: %s\n", strerror(len));
        status = 1;
    }
    return status;
}
