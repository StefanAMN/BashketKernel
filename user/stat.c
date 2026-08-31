/* stat: describe files.
 *   stat [-L] PATH...     type, size, links, inode, permissions, owner, time
 * A symbolic link is described itself; with -L, what it points to. */
#include "ulib.h"

/* The name of a struct stat type */
static const char *type_name(uint32_t type) {
    switch (type) {
        case S_FILE: return "file";
        case S_DIR: return "directory";
        case S_LINK: return "symbolic link";
        case S_PIPE: return "pipe";
        case S_SOCKET: return "socket";
        case S_TERMINAL: return "terminal";
        default: return "?";
    }
}

int main(int argc, char **argv) {
    int follow = argc > 1 && strcmp(argv[1], "-L") == 0;
    if (argc < 2 + follow) {
        fprintf(2, "usage: stat [-L] PATH...\n");
        return 2;
    }
    int status = 0;
    for (int i = 1 + follow; i < argc; i++) {
        struct stat st;
        long err = follow ? stat(argv[i], &st) : lstat(argv[i], &st);
        if (err < 0) {
            fprintf(2, "stat: %s: %s\n", argv[i], strerror(err));
            status = 1;
            continue;
        }
        char when[20] = "not known";
        if (st.mtime != 0) format_date(st.mtime, when);
        static const char *const filesystems[] = { "-", "memory", "FAT", "ext2" };
        printf("  File: %s\n  Type: %s  Size: %lu  Links: %u  Inode: %lu  On: %s\n  Mode: %04o  Owner: %u  Group: %u\n  Modified: %s\n",
               argv[i], type_name(st.type), (unsigned long)st.size, st.links, (unsigned long)st.ino,
               st.dev < 4 ? filesystems[st.dev] : "?", st.mode, st.uid, st.gid, when);
    }
    return status;
}
