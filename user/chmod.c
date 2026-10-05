/* chmod: change who may read and write a file.
 *
 *   chmod MODE FILE...     MODE is octal: 0600 owner only, 0644 others may
 *                          read, 0666 others may read and write
 */
#include "ulib.h"

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(2, "usage: chmod MODE FILE...   (MODE in octal, e.g. 0644)\n");
        return 2;
    }

    unsigned mode = 0;
    for (const char *p = argv[1]; *p; p++) {
        if (*p < '0' || *p > '7') {
            fprintf(2, "chmod: '%s' is not an octal number\n", argv[1]);
            return 2;
        }
        mode = mode * 8 + (unsigned)(*p - '0');
    }

    int status = 0;
    for (int i = 2; i < argc; i++) {
        long err = chmod(argv[i], mode);
        if (err < 0) {
            fprintf(2, "chmod: %s: %s\n", argv[i], strerror(err));
            status = 1;
        }
    }
    return status;
}
