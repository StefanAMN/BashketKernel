/* mkdir: create directories. */
#include "ulib.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(2, "usage: mkdir path...\n");
        return 2;
    }

    int status = 0;
    for (int i = 1; i < argc; i++) {
        long err = mkdir(argv[i]);
        if (err < 0) {
            fprintf(2, "mkdir: %s: %s\n", argv[i], strerror(err));
            status = 1;
        }
    }
    return status;
}
