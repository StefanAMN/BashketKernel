/* tee: copy standard input to standard output and to files.
 *   tee [-a] FILE...    write everything read to each FILE as well,
 *                       replacing them (-a: adding at their ends)
 * A FILE that cannot be opened or written is reported and left out; the
 * others still get everything, and the status is then 1. At most 16
 * files.
 */
#include "ulib.h"

#define MAX_FILES 16

/* Write all `n` bytes of `data` to `fd`, however many calls it takes.
 * 1 if they were all written, 0 on an error. */
static int write_all(int fd, const char *data, size_t n) {
    while (n > 0) {
        long done = write(fd, data, n);
        if (done <= 0) return 0;
        data += done;
        n -= (size_t)done;
    }
    return 1;
}

int main(int argc, char **argv) {
    int arg = 1, append = 0;
    if (arg < argc && strcmp(argv[arg], "-a") == 0) {
        append = 1;
        arg++;
    }
    if (argc - arg > MAX_FILES || (arg < argc && argv[arg][0] == '-' && argv[arg][1] != '\0')) {
        fprintf(2, "usage: tee [-a] FILE... (at most %d)\n", MAX_FILES);
        return 2;
    }

    int status = 0;
    int fds[MAX_FILES];
    const char *names[MAX_FILES];
    int count = 0;
    for (; arg < argc; arg++) {
        long fd = open(argv[arg], O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC));
        if (fd < 0) {
            fprintf(2, "tee: %s: %s\n", argv[arg], strerror(fd));
            status = 1;
            continue;
        }
        names[count] = argv[arg];
        fds[count++] = (int)fd;
    }

    static char buf[4096];
    long n;
    while ((n = read(0, buf, sizeof(buf))) > 0) {
        if (!write_all(1, buf, (size_t)n)) status = 1;   /* (Keep going: the files still want it) */
        for (int i = 0; i < count; i++) {
            if (fds[i] >= 0 && !write_all(fds[i], buf, (size_t)n)) {
                fprintf(2, "tee: %s: cannot write\n", names[i]);
                close(fds[i]);
                fds[i] = -1;                              /* Reported once, then left out */
                status = 1;
            }
        }
    }
    if (n < 0) {
        fprintf(2, "tee: %s\n", strerror(n));
        status = 1;
    }
    for (int i = 0; i < count; i++) if (fds[i] >= 0) close(fds[i]);
    return status;
}
