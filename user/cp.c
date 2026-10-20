/* cp: copy a file.
 *   cp SOURCE DEST        DEST is the copy, or a directory to put it in
 * The copy gets the source's permission bits (not set-uid) and belongs to
 * whoever runs cp. Copying a file onto itself is refused: it would empty
 * it before reading it. */
#include "ulib.h"

/* DEST, or DEST/(last part of SOURCE) if DEST is a directory, into
 * out[256]. 0, or -ENAMETOOLONG. */
static long target_path(const char *source, const char *dest, char out[256]) {
    struct stat st;
    size_t d = strlen(dest);
    if (stat(dest, &st) != 0 || st.type != S_DIR) {
        if (d >= 256) return -ENAMETOOLONG;
        memcpy(out, dest, d + 1);
        return 0;
    }
    const char *base = source;
    for (const char *c = source; *c; c++) if (*c == '/' && c[1]) base = c + 1;
    size_t b = strlen(base);
    while (b > 0 && base[b - 1] == '/') b--;
    if (d + 1 + b >= 256) return -ENAMETOOLONG;
    memcpy(out, dest, d);
    out[d] = '/';
    memcpy(out + d + 1, base, b);
    out[d + 1 + b] = '\0';
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(2, "usage: cp SOURCE DEST\n");
        return 2;
    }
    char dest[256];
    long err = target_path(argv[1], argv[2], dest);
    struct stat from, to;
    long in = err < 0 ? err : open(argv[1], O_RDONLY);
    if (in >= 0 && (err = fstat((int)in, &from)) == 0 && from.type == S_DIR) err = -EISDIR;
    if (in < 0 || err < 0) {
        fprintf(2, "cp: %s: %s\n", argv[1], strerror(in < 0 ? in : err));
        return 1;
    }
    if (stat(dest, &to) == 0 && to.dev == from.dev && to.ino == from.ino && from.ino != 0) {
        fprintf(2, "cp: %s and %s are the same file\n", argv[1], dest);
        return 1;
    }

    long out = open(dest, O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0) {
        fprintf(2, "cp: %s: %s\n", dest, strerror(out));
        return 1;
    }
    static char buf[4096];
    long n;
    while ((n = read((int)in, buf, sizeof(buf))) > 0) {
        long written = write((int)out, buf, (size_t)n);
        if (written != n) {
            fprintf(2, "cp: %s: %s\n", dest, written < 0 ? strerror(written) : "disk full");
            return 1;
        }
    }
    if (n < 0) {
        fprintf(2, "cp: %s: %s\n", argv[1], strerror(n));
        return 1;
    }
    close((int)out);
    close((int)in);
    long mode_err = chmod(dest, from.mode & 0777);
    if (mode_err < 0 && mode_err != -EPERM) {           /* (Copied onto someone else's file: their mode stays) */
        fprintf(2, "cp: %s: %s\n", dest, strerror(mode_err));
        return 1;
    }
    return 0;
}
