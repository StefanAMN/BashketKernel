/* fsstress: change a directory tree at random, for ever.
 *
 *   fsstress DIRECTORY [SEED]
 *
 * Creates, appends to, truncates, renames and removes files and directories
 * under DIRECTORY, as fast as it can. It never finishes: it is what
 * make test-crash has running when it pulls the plug, to see what state an
 * interrupted change leaves the filesystem in.
 */
#include "ulib.h"

#define NAMES 24

static unsigned long state;

static unsigned next(unsigned limit) {
    state = state * 6364136223846793005UL + 1442695040888963407UL;
    return (unsigned)((state >> 33) % limit);
}

static char base[96];

/* "DIRECTORY/sub3/file 17" or "DIRECTORY/file 17" */
static void name_of(char *out, unsigned n, const char *kind) {
    unsigned at = 0;
    for (const char *p = base; *p; p++) out[at++] = *p;
    out[at++] = '/';
    if (n % 3 == 0) {
        for (const char *p = "sub"; *p; p++) out[at++] = *p;
        out[at++] = (char)('0' + n % 4);
        out[at++] = '/';
    }
    for (const char *p = kind; *p; p++) out[at++] = *p;
    out[at++] = ' ';
    out[at++] = (char)('0' + n / 10);
    out[at++] = (char)('0' + n % 10);
    out[at] = '\0';
}

int main(int argc, char **argv) {
    if (argc < 2 || strlen(argv[1]) > 60) {
        fprintf(2, "usage: fsstress DIRECTORY [SEED]\n");
        return 2;
    }
    strcpy(base, argv[1]);
    state = argc > 2 ? (unsigned long)atoi(argv[2]) : 1;

    static char data[8192], from[128], to[128];
    for (unsigned i = 0; i < sizeof(data); i++) data[i] = (char)('a' + i % 26);

    /* The subdirectories the names above use */
    for (unsigned d = 0; d < 4; d++) {
        name_of(from, 0, "x");
        char *slash = from + strlen(base) + 1;
        slash[3] = (char)('0' + d);
        slash[4] = '\0';
        mkdir(from);
    }

    printf("fsstress: running in %s\n", base);
    for (unsigned long ops = 0; ; ops++) {
        unsigned n = next(NAMES), what = next(100);
        name_of(from, n, "file");

        if (what < 35) {                         /* Create or replace, with up to 24 KiB */
            long fd = open(from, O_WRONLY | O_CREAT | O_TRUNC);
            if (fd >= 0) {
                for (unsigned parts = next(4); parts > 0; parts--) write((int)fd, data, next(sizeof(data)) + 1);
                close((int)fd);
            }
        } else if (what < 55) {                  /* Append */
            long fd = open(from, O_WRONLY | O_APPEND);
            if (fd >= 0) {
                write((int)fd, data, next(sizeof(data)) + 1);
                close((int)fd);
            }
        } else if (what < 70) {                  /* A new name, perhaps in another directory */
            name_of(to, next(NAMES), "file");
            rename(from, to);
        } else if (what < 88) {
            unlink(from);
        } else if (what < 94) {                  /* A directory comes... */
            name_of(to, n, "dir");
            mkdir(to);
        } else {                                 /* ...and goes */
            name_of(to, n, "dir");
            unlink(to);
        }
        if (ops % 200 == 199) write(1, ".", 1);
    }
}
