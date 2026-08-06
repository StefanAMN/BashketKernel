/* mv: rename a file or directory, or move it to another directory.
 *
 *   mv FROM TO         TO is the new name, or a directory to move FROM into
 *
 * Only within one filesystem (a move from /tmp to /disk would be a copy and
 * a delete), and never onto something that exists already.
 */
#include "ulib.h"

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(2, "usage: mv FROM TO\n");
        return 2;
    }

    /* "mv file dir" means "dir/file": find out whether TO is a directory by
     * trying to list it */
    static char target[256];
    const char *to = argv[2];
    struct dirent entry;
    long fd = open(argv[2], O_RDONLY);
    if (fd >= 0) {
        if (readdir((int)fd, &entry) >= 0) {
            const char *leaf = argv[1];
            for (const char *p = argv[1]; *p; p++) if (*p == '/' && p[1] != '\0') leaf = p + 1;
            size_t dir_len = strlen(argv[2]), leaf_len = strlen(leaf);
            if (dir_len + 1 + leaf_len < sizeof(target)) {
                memcpy(target, argv[2], dir_len);
                if (dir_len > 0 && target[dir_len - 1] != '/') target[dir_len++] = '/';
                memcpy(target + dir_len, leaf, leaf_len + 1);
                to = target;
            }
        }
        close((int)fd);
    }

    long err = rename(argv[1], to);
    if (err < 0) {
        fprintf(2, "mv: %s -> %s: %s\n", argv[1], to, strerror(err));
        return 1;
    }
    return 0;
}
