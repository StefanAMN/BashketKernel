/* head: the first lines of a file or of standard input.
 *   head [-n N] [FILE]      the first N lines (10 if not given) */
#include "ulib.h"
#include "lines.h"

int main(int argc, char **argv) {
    long count = 10;
    int arg = 1;
    if (arg + 1 < argc && strcmp(argv[arg], "-n") == 0) {
        count = atoi(argv[arg + 1]);
        arg += 2;
    }
    if (count < 0 || arg + 1 < argc) {
        fprintf(2, "usage: head [-n N] [FILE]\n");
        return 2;
    }
    long fd = arg < argc ? open(argv[arg], O_RDONLY) : 0;
    if (fd < 0) {
        fprintf(2, "head: %s: %s\n", argv[arg], strerror(fd));
        return 1;
    }

    static lines_t in;
    lines_open(&in, (int)fd);
    long len = 0;
    for (long i = 0; i < count && (len = lines_next(&in)) >= 0; i++) {
        write(1, in.line, (size_t)len);
        write(1, "\n", 1);
    }
    lines_close(&in);
    if (len < -1) {
        fprintf(2, "head: %s\n", strerror(len));
        return 1;
    }
    return 0;
}
