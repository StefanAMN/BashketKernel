/* mkdir: create directories.
 *   mkdir [-p] PATH...
 * With -p the directories on the way that are missing are made first, and
 * one that exists already is not an error (if it is a directory). Exit
 * status 1 if any could not be made, 2 for a wrong option. */
#include "ulib.h"

/* Whether `path` names a directory (following links, as mkdir -p walks
 * through them) */
static int is_directory(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && st.type == S_DIR;
}

/* mkdir -p for one path: make each missing directory from the top down.
 * 0, or the error of the first that could not be made (reported). */
static long make_parents(const char *path) {
    char partial[256];
    size_t len = strlen(path);
    if (len >= sizeof(partial)) {
        fprintf(2, "mkdir: %s: %s\n", path, strerror(-ENAMETOOLONG));
        return -ENAMETOOLONG;
    }
    memcpy(partial, path, len + 1);
    /* Each '/' after a name ends a prefix; the whole path is the last one */
    for (size_t i = 1; i <= len; i++) {
        if (i < len && (partial[i] != '/' || partial[i - 1] == '/')) continue;
        char saved = partial[i];
        partial[i] = '\0';
        long err = mkdir(partial);
        if (err == -EEXIST && is_directory(partial)) err = 0;
        if (err < 0) {
            fprintf(2, "mkdir: %s: %s\n", partial, strerror(err));
            return err;
        }
        partial[i] = saved;
    }
    return 0;
}

int main(int argc, char **argv) {
    int parents = 0, first = 1;
    if (argc > 1 && argv[1][0] == '-') {
        if (strcmp(argv[1], "-p") != 0) {
            fprintf(2, "mkdir: unknown option %s\nusage: mkdir [-p] path...\n", argv[1]);
            return 2;
        }
        parents = 1;
        first = 2;
    }
    if (first >= argc) {
        fprintf(2, "usage: mkdir [-p] path...\n");
        return 2;
    }

    int status = 0;
    for (int i = first; i < argc; i++) {
        if (parents) {
            if (make_parents(argv[i]) < 0) status = 1;
            continue;
        }
        long err = mkdir(argv[i]);
        if (err < 0) {
            fprintf(2, "mkdir: %s: %s\n", argv[i], strerror(err));
            status = 1;
        }
    }
    return status;
}
