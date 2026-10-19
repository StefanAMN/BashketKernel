/* cat: copy files (or standard input, if none are named) to standard output. */
#include "ulib.h"

/* Copy everything readable from `fd` to standard output. 0, or 1 if a read
 * or write failed */
static int copy(int fd) {
    char buf[512];
    long n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        long sent = 0;
        while (sent < n) {
            long w = write(1, buf + sent, (size_t)(n - sent));
            if (w <= 0) return 1;
            sent += w;
        }
    }
    return n < 0 ? 1 : 0;
}

int main(int argc, char **argv) {
    if (argc < 2) return copy(0);

    int status = 0;
    for (int i = 1; i < argc; i++) {
        long fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            fprintf(2, "cat: %s: %s\n", argv[i], strerror(fd));
            status = 1;
            continue;
        }
        if (copy((int)fd) != 0) {
            fprintf(2, "cat: %s: read error\n", argv[i]);
            status = 1;
        }
        close((int)fd);
    }
    return status;
}
