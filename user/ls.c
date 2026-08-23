/* ls: list a directory (default: the current one).
 *   ls [-l] [DIRECTORY]      -l also shows permissions, owner and size
 * A symbolic link is shown with what it points to: "name -> target". */
#include "ulib.h"

/* The target of the link `name` in the directory `dir`, into target[256]
 * (NUL-terminated; "?" if it cannot be read) */
static void link_target(const char *dir, const char *name, char target[256]) {
    char path[256];
    size_t d = strlen(dir), n = strlen(name);
    long len = -1;
    if (d + 1 + n < sizeof(path)) {
        memcpy(path, dir, d);
        path[d] = '/';
        memcpy(path + d + 1, name, n + 1);
        len = readlink(path, target, 255);
    }
    if (len < 0) len = 0, target[len++] = '?';
    target[len] = '\0';
}

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
        char target[256] = "";
        if (entry.type == DT_LNK) link_target(path, entry.name, target);
        if (!long_format) {
            if (entry.type == DT_DIR) printf("%s/\n", entry.name);
            else if (entry.type == DT_LNK) printf("%s -> %s\n", entry.name, target);
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
            format_date(entry.mtime, when);
            when[16] = '\0';                       /* To the minute */
        }
        printf("%c%s %5u %5u %8lu %s %s%s%s\n", entry.type == DT_DIR ? 'd' : entry.type == DT_LNK ? 'l' : '-',
               perms, entry.uid, entry.gid, (unsigned long)entry.size, when, entry.name,
               entry.type == DT_LNK ? " -> " : "", target);
    }

    if (result < 0) {
        fprintf(2, "ls: %s: %s\n", path, strerror(result));
        return 1;
    }
    close((int)fd);
    return 0;
}
