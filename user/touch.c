/* touch: mark files as changed now, making them if they do not exist.
 *   touch [-d SECONDS] FILE...    -d: that time (seconds since 1970) instead of now */
#include "ulib.h"

int main(int argc, char **argv) {
    unsigned long when = UTIME_NOW;
    int arg = 1;
    if (arg + 1 < argc && strcmp(argv[arg], "-d") == 0) {
        when = (unsigned long)atoi(argv[arg + 1]);
        arg += 2;
    }
    if (arg >= argc) {
        fprintf(2, "usage: touch [-d SECONDS] FILE...\n");
        return 2;
    }
    int status = 0;
    for (; arg < argc; arg++) {
        struct stat st;
        if (stat(argv[arg], &st) == -ENOENT) {        /* Not there: an empty file */
            long fd = open(argv[arg], O_WRONLY | O_CREAT);
            if (fd < 0) {
                fprintf(2, "touch: %s: %s\n", argv[arg], strerror(fd));
                status = 1;
                continue;
            }
            close((int)fd);
        }
        long err = utime(argv[arg], when);
        if (err < 0) {
            fprintf(2, "touch: %s: %s\n", argv[arg], strerror(err));
            status = 1;
        }
    }
    return status;
}
