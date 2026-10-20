/* uniq: lines of a file or of standard input, with repeats in a row shown
 * once.
 *   uniq [-c] [-d] [FILE]
 * Only neighbouring lines are compared, so a sorted input ("sort | uniq")
 * shows every different line once. -c puts how many times each line came
 * in a row before it ("3 text"); -d shows only lines that came more than
 * once. Lines may be of any length (user/lib/lines.h). */
#include "ulib.h"
#include "lines.h"

static int show_count = 0;
static int only_repeated = 0;

/* Write one output line: `line` (`len` bytes) seen `times` in a row */
static void emit(const char *line, long len, long times) {
    if (only_repeated && times < 2) return;
    if (show_count) printf("%ld ", times);
    write(1, line, (size_t)len);
    write(1, "\n", 1);
}

int main(int argc, char **argv) {
    int arg = 1;
    for (; arg < argc && argv[arg][0] == '-' && argv[arg][1]; arg++) {
        if (strcmp(argv[arg], "-c") == 0) show_count = 1;
        else if (strcmp(argv[arg], "-d") == 0) only_repeated = 1;
        else break;
    }
    if (arg + 1 < argc || (arg < argc && argv[arg][0] == '-' && argv[arg][1])) {
        fprintf(2, "usage: uniq [-c] [-d] [FILE]\n");
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
        if (times > 0 && len == kept_len && memcmp(in.line, kept, (size_t)len) == 0) {
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
