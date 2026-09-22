/* rm: remove files and empty directories.
 *   rm [-r] PATH...
 * With -r a directory is removed with everything under it. Symbolic links
 * are removed, never followed: a link to a directory elsewhere leaves that
 * directory alone. "/", and a path whose last part is "." or "..", are
 * refused; so is going more than RM_DEPTH levels down. Exit status 1 if
 * anything could not be removed (what could be is removed), 2 for a wrong
 * option. */
#include "ulib.h"

#define RM_DEPTH 64          /* Levels below a PATH entered at most */

static int status;           /* The exit status: 1 after a problem */

/* Report a failure to remove `path` and remember it in the status */
static void failed(const char *path, long err) {
    fprintf(2, "rm: %s: %s\n", path, strerror(err));
    status = 1;
}

/* Whether -r must refuse `path`: "/" (any number of slashes), or a last
 * part "." or ".." (which would remove the directory being stood in, or
 * its parent) */
static int forbidden(const char *path) {
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/') n--;              /* "a/b/" is "a/b" */
    if (n == 1 && path[0] == '/') return 1;
    size_t start = n;
    while (start > 0 && path[start - 1] != '/') start--;
    size_t last = n - start;
    return (last == 1 && path[start] == '.') ||
           (last == 2 && path[start] == '.' && path[start + 1] == '.');
}

/* The names in the directory `path`, each ending in a NUL, in a buffer
 * from malloc (*count of them), read in full and the directory closed
 * before anything is removed: removing while reading could skip entries,
 * and a deep tree would run out of descriptors. NULL if it cannot be read
 * (reported). */
static char *read_names(const char *path, int *count) {
    long fd = open(path, O_RDONLY);
    if (fd < 0) {
        failed(path, fd);
        return NULL;
    }
    size_t used = 0, cap = 256;
    char *names = malloc(cap);
    struct dirent entry;
    *count = 0;
    while (names && readdir((int)fd, &entry) == 1) {
        size_t n = strlen(entry.name) + 1;
        if (used + n > cap) {                          /* Twice the room */
            char *bigger = malloc(cap * 2 + n);
            if (bigger) memcpy(bigger, names, used);
            free(names);
            names = bigger;
            cap = cap * 2 + n;
            if (!names) break;
        }
        memcpy(names + used, entry.name, n);
        used += n;
        (*count)++;
    }
    close((int)fd);
    if (!names) failed(path, -ENOMEM);
    return names;
}

/* Remove `path` and, if it is a directory (not a link to one), everything
 * under it first; `depth` levels below the PATH given */
static void remove_tree(const char *path, int depth) {
    struct stat st;
    long err = lstat(path, &st);                  /* A link is described, not followed */
    if (err < 0) {
        failed(path, err);
        return;
    }
    if (st.type == S_DIR) {
        if (depth >= RM_DEPTH) {
            fprintf(2, "rm: %s: more than %d levels down, not entered\n", path, RM_DEPTH);
            status = 1;
            return;
        }
        int count = 0;
        char *names = read_names(path, &count);
        if (!names) return;
        const char *name = names;
        for (int i = 0; i < count; i++, name += strlen(name) + 1) {
            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
            char child[256];
            size_t p = strlen(path), n = strlen(name);
            int slash = p > 0 && path[p - 1] != '/';
            if (p + slash + n >= sizeof(child)) {
                fprintf(2, "rm: %s/%s: path too long\n", path, name);
                status = 1;
                continue;
            }
            memcpy(child, path, p);
            if (slash) child[p] = '/';
            memcpy(child + p + slash, name, n + 1);
            remove_tree(child, depth + 1);
        }
        free(names);
    }
    err = unlink(path);
    if (err < 0) failed(path, err);
}

int main(int argc, char **argv) {
    int recursive = 0, first = 1;
    if (argc > 1 && argv[1][0] == '-') {
        if (strcmp(argv[1], "-r") != 0) {
            fprintf(2, "rm: unknown option %s\nusage: rm [-r] path...\n", argv[1]);
            return 2;
        }
        recursive = 1;
        first = 2;
    }
    if (first >= argc) {
        fprintf(2, "usage: rm [-r] path...\n");
        return 2;
    }

    for (int i = first; i < argc; i++) {
        if (!recursive) {
            long err = unlink(argv[i]);
            if (err < 0) failed(argv[i], err);
        } else if (forbidden(argv[i])) {
            fprintf(2, "rm: refusing to remove %s\n", argv[i]);
            status = 1;
        } else {
            remove_tree(argv[i], 0);
        }
    }
    return status;
}
