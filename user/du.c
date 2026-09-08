/* du: how much space is under a directory.
 *   du [-s] [DIR]
 * For DIR (default ".") and every directory below it, the KiB of
 * everything in it (each file, link and directory counted as its size
 * rounded up to whole KiB), one "KiB<tab>path" line each, deepest first;
 * with -s only DIR's total. Symbolic links are counted, not followed; a
 * directory reached twice (the same device and inode) is counted once,
 * and nothing more than DU_DEPTH levels down is entered. A directory that
 * cannot be read is reported and left out; the exit status is then 1. */
#include "ulib.h"

#define DU_DEPTH 32            /* Levels below DIR entered at most */
#define DU_VISITED 1024        /* Directories remembered, to count each once */

static int summary_only;           /* -s */
static int status;                 /* The exit status: 1 after a problem */
static struct { uint32_t dev; uint64_t ino; } visited[DU_VISITED];
static int visited_count;

/* Whether the directory (dev, ino) has been counted before; remembers it
 * if not. Past DU_VISITED directories, new ones are not remembered. */
static int seen_before(const struct stat *st) {
    for (int i = 0; i < visited_count; i++) {
        if (visited[i].dev == st->dev && visited[i].ino == st->ino) return 1;
    }
    if (visited_count < DU_VISITED && st->ino != 0) {
        visited[visited_count].dev = st->dev;
        visited[visited_count++].ino = st->ino;
    }
    return 0;
}

/* Whole KiB for `size` bytes, rounded up */
static unsigned long kib(uint64_t size) { return (unsigned long)((size + 1023) / 1024); }

/* The names in the directory `path`, one after another, each ending in a
 * NUL, in a buffer from malloc (*count of them). The directory is closed
 * before this returns: a walk that kept every directory on its way open
 * would run out of descriptors a dozen levels down. NULL (reported) if it
 * cannot be read. */
static char *read_names(const char *path, int *count) {
    long fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(2, "du: %s: %s\n", path, strerror(fd));
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
        fprintf(2, "du: %s: out of memory\n", path);
        status = 1;
    }
    return names;
}

/* The KiB under the directory `path` (described by st), its own size
 * included, printing a line for it (and for each directory below) unless
 * -s asks only for the start's; `depth` levels below the start */
static unsigned long total(const char *path, const struct stat *st, int depth) {
    unsigned long sum = kib(st->size);
    if (depth >= DU_DEPTH) {
        fprintf(2, "du: %s: more than %d levels down, not entered\n", path, DU_DEPTH);
        status = 1;
        return sum;
    }
    int count = 0;
    char *names = read_names(path, &count);
    if (!names) return sum;
    const char *name = names;
    for (int i = 0; i < count; i++, name += strlen(name) + 1) {
        char child[256];
        size_t p = strlen(path), n = strlen(name);
        int slash = p > 0 && path[p - 1] != '/';
        if (p + slash + n >= sizeof(child)) {
            fprintf(2, "du: %s/%s: path too long\n", path, name);
            status = 1;
            continue;
        }
        memcpy(child, path, p);
        if (slash) child[p] = '/';
        memcpy(child + p + slash, name, n + 1);

        struct stat cst;
        long err = lstat(child, &cst);
        if (err < 0) {
            fprintf(2, "du: %s: %s\n", child, strerror(err));
            status = 1;
            continue;
        }
        if (cst.type == S_DIR) {
            if (!seen_before(&cst)) sum += total(child, &cst, depth + 1);
        } else {
            sum += kib(cst.size);
        }
    }
    free(names);
    if (!summary_only || depth == 0) printf("%lu\t%s\n", sum, path);
    return sum;
}

int main(int argc, char **argv) {
    int arg = 1;
    if (arg < argc && strcmp(argv[arg], "-s") == 0) { summary_only = 1; arg++; }
    if (arg + 1 < argc) {
        fprintf(2, "usage: du [-s] [DIR]\n");
        return 2;
    }
    const char *start = arg < argc ? argv[arg] : ".";
    struct stat st;
    long err = lstat(start, &st);
    if (err < 0) {
        fprintf(2, "du: %s: %s\n", start, strerror(err));
        return 1;
    }
    if (st.type != S_DIR) {
        printf("%lu\t%s\n", kib(st.size), start);
        return 0;
    }
    seen_before(&st);
    total(start, &st, 0);
    return status;
}
