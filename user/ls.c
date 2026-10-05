/* ls: list a directory (default: the current one).
 *   ls [-l] [DIRECTORY]      -l also shows permissions, owner and size */
#include "ulib.h"

int main(int argc, char **argv) {
    int long_format = (argc > 1 && strcmp(argv[1], "-l") == 0);
    const char *path = argc > 1 + long_format ? argv[1 + long_format] : ".";

    long fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(2, "ls: %s: %s\n", path, strerror(fd));
        return 1;
    }

    struct dirent entry;
    long result;
    while ((result = readdir((int)fd, &entry)) == 1) {
        if (!long_format) {
            if (entry.type == DT_DIR) printf("%s/\n", entry.name);
            else printf("%s  (%lu bytes)\n", entry.name, (unsigned long)entry.size);
            continue;
        }

        /* rw for the owner, rw for everyone else, then the owner's uid */
        char perms[5];
        perms[0] = (entry.mode & 0400) ? 'r' : '-';
        perms[1] = (entry.mode & 0200) ? 'w' : '-';
        perms[2] = (entry.mode & 0004) ? 'r' : '-';
        perms[3] = (entry.mode & 0002) ? 'w' : '-';
        perms[4] = '\0';
        printf("%c%s %5u %8lu %s\n", entry.type == DT_DIR ? 'd' : '-', perms, entry.uid,
               (unsigned long)entry.size, entry.name);
    }

    if (result < 0) {
        fprintf(2, "ls: %s: %s\n", path, strerror(result));
        return 1;
    }
    close((int)fd);
    return 0;
}
