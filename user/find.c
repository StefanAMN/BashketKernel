/* find: list every path under a directory.
 *   find [DIR] [-name PATTERN] [-type f|d]
 * DIR (default ".") and everything below it, one path a line, filtered by
 * the name's last part (a wildcard pattern, as the shell's: user/lib/glob.h)
 * and by type (f: anything but a directory, d: directories). Symbolic links
 * are listed, never followed. A directory reached twice (the same device
 * and inode) is not entered again, nor is anything more than FIND_DEPTH
 * levels down. A directory that cannot be read is reported and the rest
 * still listed; the exit status is then 1. */
#include "ulib.h"
#include "glob.h"

#define FIND_DEPTH 32          /* Levels below DIR entered at most */
#define FIND_VISITED 1024      /* Directories remembered, to enter each once */

static const char *name_pattern;   /* -name, or NULL */
static int want_type;              /* -type: 'f', 'd', or 0 for both */
static int status;                 /* The exit status: 1 after a problem */
static struct { uint32_t dev; uint64_t ino; } visited[FIND_VISITED];
static int visited_count;

/* Whether the directory (dev, ino) has been entered before; remembers it
 * if not. Past FIND_VISITED directories, new ones are not remembered. */
static int seen_before(const struct stat *st) {
    for (int i = 0; i < visited_count; i++) {
        if (visited[i].dev == st->dev && visited[i].ino == st->ino) return 1;
    }
    if (visited_count < FIND_VISITED && st->ino != 0) {
        visited[visited_count].dev = st->dev;
        visited[visited_count++].ino = st->ino;
    }
    return 0;
}

/* Print `path` (described by st) if it passes the filters */
static void consider(const char *path, const struct stat *st) {
    if (want_type == 'd' && st->type != S_DIR) return;
    if (want_type == 'f' && st->type == S_DIR) return;
    if (name_pattern) {
        const char *last = path;
        for (const char *c = path; *c; c++) if (*c == '/' && c[1]) last = c + 1;
        if (!glob_match(name_pattern, last)) return;
    }
    printf("%s\n", path);
}

/* The names in the directory `path`, one after another, each ending in a
 * NUL, in a buffer from malloc (*count of them). The directory is closed
 * before this returns: a walk that kept every directory on its way open
 * would run out of descriptors a dozen levels down. NULL (reported) if it
 * cannot be read. */
static char *read_names(const char *path, int *count) {
    long fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(2, "find: %s: %s\n", path, strerror(fd));
        status = 1;
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
    if (!names) {
        fprintf(2, "find: %s: out of memory\n", path);
        status = 1;
    }
    return names;
}

/* List what is in the directory `path` (already considered), `depth`
 * levels below the start, and below it in turn */
static void walk(const char *path, int depth) {
    if (depth >= FIND_DEPTH) {
        fprintf(2, "find: %s: more than %d levels down, not entered\n", path, FIND_DEPTH);
        status = 1;
        return;
    }
    int count = 0;
    char *names = read_names(path, &count);
    if (!names) return;
    const char *name = names;
    for (int i = 0; i < count; i++, name += strlen(name) + 1) {
        char child[256];
        size_t p = strlen(path), n = strlen(name);
        int slash = p > 0 && path[p - 1] != '/';
        if (p + slash + n >= sizeof(child)) {
            fprintf(2, "find: %s/%s: path too long\n", path, name);
            status = 1;
            continue;
        }
        memcpy(child, path, p);
        if (slash) child[p] = '/';
        memcpy(child + p + slash, name, n + 1);

        struct stat st;
        long err = lstat(child, &st);
        if (err < 0) {
            fprintf(2, "find: %s: %s\n", child, strerror(err));
            status = 1;
            continue;
        }
        consider(child, &st);
        if (st.type == S_DIR && !seen_before(&st)) walk(child, depth + 1);
    }
    free(names);
}

int main(int argc, char **argv) {
    const char *start = ".";
    int arg = 1;
    if (arg < argc && argv[arg][0] != '-') start = argv[arg++];
    for (; arg < argc; arg += 2) {
        if (arg + 1 < argc && strcmp(argv[arg], "-name") == 0) {
            name_pattern = argv[arg + 1];
        } else if (arg + 1 < argc && strcmp(argv[arg], "-type") == 0 &&
                   (strcmp(argv[arg + 1], "f") == 0 || strcmp(argv[arg + 1], "d") == 0)) {
            want_type = argv[arg + 1][0];
        } else {
            fprintf(2, "usage: find [DIR] [-name PATTERN] [-type f|d]\n");
            return 2;
        }
    }

    struct stat st;
    long err = lstat(start, &st);
    if (err < 0) {
        fprintf(2, "find: %s: %s\n", start, strerror(err));
        return 1;
    }
    consider(start, &st);
    if (st.type == S_DIR && !seen_before(&st)) walk(start, 0);
    return status;
}
