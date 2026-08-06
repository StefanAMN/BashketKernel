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

        /* rwx for the owner, the group and everyone else ('s' in the
         * owner's x place: set-uid), then the owner and the group */
        char perms[10];
        for (int i = 0; i < 9; i++) perms[i] = (entry.mode & (0400u >> i)) ? "rwx"[i % 3] : '-';
        if (entry.mode & 04000) perms[2] = (entry.mode & 0100) ? 's' : 'S';
        perms[9] = '\0';
        /* When it was last written, where the filesystem records that */
        char when[20] = "                ";
        if (entry.mtime != 0) {
            unsigned date = entry.mtime >> 16, time = entry.mtime & 0xFFFF;
            unsigned parts[5] = { 1980 + (date >> 9), (date >> 5) & 15, date & 31, time >> 11, (time >> 5) & 63 };
            static const char separators[5] = { '-', '-', ' ', ':', '\0' };
            int at = 0;
            for (int i = 0; i < 5; i++) {
                int digits = i == 0 ? 4 : 2;
                for (int d = digits - 1; d >= 0; d--) { when[at + d] = (char)('0' + parts[i] % 10); parts[i] /= 10; }
                at += digits;
                when[at++] = separators[i];
            }
        }
        printf("%c%s %5u %5u %8lu %s %s\n", entry.type == DT_DIR ? 'd' : '-', perms, entry.uid, entry.gid,
               (unsigned long)entry.size, when, entry.name);
    }

    if (result < 0) {
        fprintf(2, "ls: %s: %s\n", path, strerror(result));
        return 1;
    }
    close((int)fd);
    return 0;
}
