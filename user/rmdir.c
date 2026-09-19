/* rmdir: remove empty directories.
 *   rmdir PATH...
 * Only directories, and only empty ones (a file or a link to a directory
 * is refused, unlike with rm). Exit status 1 if any could not be removed. */
#include "ulib.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(2, "usage: rmdir path...\n");
        return 2;
    }

    int status = 0;
    for (int i = 1; i < argc; i++) {
        struct stat st;
        long err = lstat(argv[i], &st);
        if (err == 0 && st.type != S_DIR) err = -ENOTDIR;
        if (err == 0) err = unlink(argv[i]);   /* Refused by the kernel if not empty */
        if (err < 0) {
            fprintf(2, "rmdir: %s: %s\n", argv[i], strerror(err));
            status = 1;
        }
    }
    return status;
}
