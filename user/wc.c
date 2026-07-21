/* wc: count lines, words and bytes of a file or of standard input. */
#include "ulib.h"

int main(int argc, char **argv) {
    long fd = 0;
    if (argc > 1) {
        fd = open(argv[1], O_RDONLY);
        if (fd < 0) {
            fprintf(2, "wc: %s: %s\n", argv[1], strerror(fd));
            return 1;
        }
    }

    unsigned long lines = 0, words = 0, bytes = 0;
    int in_word = 0;
    char buf[512];
    long n;

    while ((n = read((int)fd, buf, sizeof(buf))) > 0) {
        bytes += (unsigned long)n;
        for (long i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n') lines++;
            if (c == ' ' || c == '\n' || c == '\t') {
                in_word = 0;
            } else if (!in_word) {
                in_word = 1;
                words++;
            }
        }
    }

    printf("%lu %lu %lu\n", lines, words, bytes);
    return n < 0 ? 1 : 0;
}
