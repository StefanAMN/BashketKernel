/* grep: print the lines that contain a text.
 *   grep [-v] [-c] TEXT [FILE...]
 * -v prints the lines that do not contain it, -c only counts them. The text
 * is matched as it is (no regular expressions). With several files each
 * line is preceded by its file's name. Exit status: 0 if a line was
 * selected, 1 if none was, 2 on an error. */
#include "ulib.h"
#include "lines.h"

/* Whether `line` (len bytes, may hold NULs) contains `text` (n bytes) */
static int contains(const char *line, long len, const char *text, long n) {
    for (long i = 0; i + n <= len; i++) {
        if (memcmp(line + i, text, (size_t)n) == 0) return 1;
    }
    return 0;
}

/* Search one open file; prints as asked. The number of lines selected, or
 * a negative error number. */
static long search(int fd, const char *name, const char *text, int invert, int count_only) {
    static lines_t in;
    lines_open(&in, fd);
    long n = (long)strlen(text), selected = 0, len;
    while ((len = lines_next(&in)) >= 0) {
        if (contains(in.line, len, text, n) == invert) continue;
        selected++;
        if (count_only) continue;
        if (name) printf("%s:", name);
        write(1, in.line, (size_t)len);                /* (printf would stop at a NUL) */
        write(1, "\n", 1);
    }
    lines_close(&in);
    if (len < -1) return len;
    if (count_only) {
        if (name) printf("%s:", name);
        printf("%ld\n", selected);
    }
    return selected;
}

int main(int argc, char **argv) {
    int invert = 0, count_only = 0, arg = 1;
    for (; arg < argc && argv[arg][0] == '-' && argv[arg][1] != '\0'; arg++) {
        if (strcmp(argv[arg], "-v") == 0) invert = 1;
        else if (strcmp(argv[arg], "-c") == 0) count_only = 1;
        else break;
    }
    if (arg >= argc) {
        fprintf(2, "usage: grep [-v] [-c] TEXT [FILE...]\n");
        return 2;
    }
    const char *text = argv[arg++];
    int files = argc - arg, status = 1;
    if (files == 0) {
        long found = search(0, NULL, text, invert, count_only);
        if (found < 0) { fprintf(2, "grep: %s\n", strerror(found)); return 2; }
        return found > 0 ? 0 : 1;
    }
    for (; arg < argc; arg++) {
        long fd = open(argv[arg], O_RDONLY);
        long found = fd >= 0 ? search((int)fd, files > 1 ? argv[arg] : NULL, text, invert, count_only) : fd;
        if (fd >= 0) close((int)fd);
        if (found < 0) {
            fprintf(2, "grep: %s: %s\n", argv[arg], strerror(found));
            status = 2;
        } else if (found > 0 && status == 1) {
            status = 0;
        }
    }
    return status;
}
