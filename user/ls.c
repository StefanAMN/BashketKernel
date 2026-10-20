/* ls: list a directory (default: the current one).
 *   ls [-l] [DIRECTORY]      -l also shows permissions, owner and size
 * A symbolic link is shown with what it points to: "name -> target". */
#include "ulib.h"

/* "dir/name" into path[256]: 0, or -1 if it does not fit */
static int join(const char *dir, const char *name, char path[256]) {
    size_t d = strlen(dir), n = strlen(name);
    if (d + 1 + n >= 256) return -1;
    memcpy(path, dir, d);
    path[d] = '/';
    memcpy(path + d + 1, name, n + 1);
    return 0;
}

/* The target of the link `name` in the directory `dir`, into target[256]
 * (NUL-terminated; "?" if it cannot be read) */
static void link_target(const char *dir, const char *name, char target[256]) {
    char path[256];
    long len = join(dir, name, path) == 0 ? readlink(path, target, 255) : -1;
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

        /* The entry as lstat describes it (the link count is only there);
         * what the directory listing carries if that fails */
        char full[256];
        struct stat st;
        unsigned links = 1;
        if (join(path, entry.name, full) == 0 && lstat(full, &st) == 0) {
            entry.mode = st.mode;
            entry.uid = st.uid;
            entry.gid = st.gid;
            entry.size = st.size;
            entry.mtime = st.mtime;
            links = st.links;
        }

        /* rwx for the owner, the group and everyone else ('s' in the
         * owner's x place: set-uid, in the group's: set-gid; 't' in the
         * last: sticky; upper case when the x under it is not set), then
         * the owner and the group */
        char perms[10];
        for (int i = 0; i < 9; i++) perms[i] = (entry.mode & (0400u >> i)) ? "rwx"[i % 3] : '-';
        if (entry.mode & 04000) perms[2] = (entry.mode & 0100) ? 's' : 'S';
        if (entry.mode & 02000) perms[5] = (entry.mode & 0010) ? 's' : 'S';
        if (entry.mode & 01000) perms[8] = (entry.mode & 0001) ? 't' : 'T';
        perms[9] = '\0';
        /* When it was last written, where the filesystem records that */
        char when[20] = "                ";
        if (entry.mtime != 0) {
            format_date(entry.mtime, when);
            when[16] = '\0';                       /* To the minute */
        }
        printf("%c%s %3u %5u %5u %8lu %s %s%s%s\n", entry.type == DT_DIR ? 'd' : entry.type == DT_LNK ? 'l' : '-',
               perms, links, entry.uid, entry.gid, (unsigned long)entry.size, when, entry.name,
               entry.type == DT_LNK ? " -> " : "", target);
    }

    if (result < 0) {
        fprintf(2, "ls: %s: %s\n", path, strerror(result));
        return 1;
    }
    close((int)fd);
    return 0;
}
