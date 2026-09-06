/* truncate: make files a given length.
 *   truncate -s LENGTH FILE...    shorter: the end is cut off; longer: zeros added
 * A file that does not exist is made. */
#include "ulib.h"

int main(int argc, char **argv) {
    if (argc < 4 || strcmp(argv[1], "-s") != 0 || argv[2][0] < '0' || argv[2][0] > '9') {
        fprintf(2, "usage: truncate -s LENGTH FILE...\n");
        return 2;
    }
    unsigned long length = (unsigned long)atoi(argv[2]);
    int status = 0;
    for (int i = 3; i < argc; i++) {
        long fd = open(argv[i], O_WRONLY | O_CREAT);
        long err = fd < 0 ? fd : ftruncate((int)fd, length);
        if (fd >= 0) close((int)fd);
        if (err < 0) {
            fprintf(2, "truncate: %s: %s\n", argv[i], strerror(err));
            status = 1;
        }
    }
    return status;
}
