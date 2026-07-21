/* ls: list a directory (default "/"). */
#include "ulib.h"

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "/";

    long fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(2, "ls: %s: %s\n", path, strerror(fd));
        return 1;
    }

    struct dirent entry;
    long result;
    while ((result = readdir((int)fd, &entry)) == 1) {
        if (entry.type == DT_DIR) printf("%s/\n", entry.name);
        else printf("%s  (%lu bytes)\n", entry.name, (unsigned long)entry.size);
    }

    if (result < 0) {
        fprintf(2, "ls: %s: %s\n", path, strerror(result));
        return 1;
    }
    close((int)fd);
    return 0;
}
