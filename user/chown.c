/* chown: change the owner and group of files.
 *
 *   chown UID[:GID] FILE...     numbers; chown :GID FILE changes only the group
 *
 * Giving files away is root's privilege. An owner may move a file of theirs
 * into their own group.
 */
#include "ulib.h"

static int number(const char *text, unsigned *out) {
    if (*text == '\0') return -1;
    unsigned value = 0;
    for (; *text; text++) {
        if (*text < '0' || *text > '9') return -1;
        value = value * 10 + (unsigned)(*text - '0');
    }
    *out = value;
    return 0;
}

int main(int argc, char **argv) {
    unsigned uid = KEEP_ID, gid = KEEP_ID;
    int ok = argc >= 3;
    if (ok) {
        char *colon = strchr(argv[1], ':');
        if (colon) {
            *colon = '\0';
            ok = number(colon + 1, &gid) == 0;
        }
        if (ok && argv[1][0] != '\0') ok = number(argv[1], &uid) == 0;
        if (uid == KEEP_ID && gid == KEEP_ID) ok = 0;
    }
    if (!ok) {
        fprintf(2, "usage: chown UID[:GID] FILE...   or   chown :GID FILE...\n");
        return 2;
    }

    int status = 0;
    for (int i = 2; i < argc; i++) {
        long err = chown(argv[i], uid, gid);
        if (err < 0) {
            fprintf(2, "chown: %s: %s\n", argv[i], strerror(err));
            status = 1;
        }
    }
    return status;
}
