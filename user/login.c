/* login: check a password and start a shell as that user.
 *
 *   login NAME
 *
 * Must be run as root: it reads the password file, which only root can, and
 * changes user id, which only root may. After the switch the shell (and
 * everything it starts) has that user's rights and no way back to root;
 * leaving the shell returns to whoever ran login.
 *
 * /etc/passwd holds one line per user: name:uid:hash, where hash is the
 * SHA-256 of the password, written in hexadecimal. Storing a hash means the
 * file does not contain the passwords themselves. (A real system would also
 * mix a random "salt" into each hash and use a deliberately slow function, so
 * that equal passwords do not give equal hashes and guessing is expensive.)
 */
#include "ulib.h"
#include "sha256.h"

/* Read a line from the terminal without showing it */
static long read_password(char *buf, size_t size) {
    size_t len = 0;
    tty_raw(1);
    for (;;) {
        char c;
        long n = read(0, &c, 1);
        if (n == -EINTR) continue;
        if (n <= 0 || c == '\r' || c == '\n') break;
        if ((c == '\b' || c == 0x7F) && len > 0) { len--; continue; }
        if (c >= 0x20 && c < 0x7F && len < size - 1) buf[len++] = c;
    }
    tty_raw(0);
    buf[len] = '\0';
    write(1, "\n", 1);
    return (long)len;
}

static void to_hex(const uint8_t *bytes, size_t count, char *out) {
    for (size_t i = 0; i < count; i++) {
        out[2 * i] = "0123456789abcdef"[bytes[i] >> 4];
        out[2 * i + 1] = "0123456789abcdef"[bytes[i] & 15];
    }
    out[2 * count] = '\0';
}

/* Find NAME in the password file; fills in the uid and hash. */
static int lookup(const char *name, long *uid, char hash[65]) {
    long fd = open("/etc/passwd", O_RDONLY);
    if (fd < 0) {
        fprintf(2, "login: /etc/passwd: %s\n", strerror(fd));
        return -1;
    }

    static char text[4096];
    long total = 0, n;
    while (total < (long)sizeof(text) - 1 && (n = read((int)fd, text + total, sizeof(text) - 1 - (size_t)total)) > 0) total += n;
    close((int)fd);
    text[total] = '\0';

    size_t name_len = strlen(name);
    for (char *line = text; *line; ) {
        char *end = strchr(line, '\n');
        if (end) *end = '\0';

        /* name:uid:hash */
        if (strncmp(line, name, name_len) == 0 && line[name_len] == ':') {
            char *uid_text = line + name_len + 1;
            char *colon = strchr(uid_text, ':');
            if (colon && strlen(colon + 1) == 64) {
                *colon = '\0';
                *uid = atoi(uid_text);
                strcpy(hash, colon + 1);
                return 0;
            }
        }
        if (!end) break;
        line = end + 1;
    }
    return -1;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(2, "usage: login NAME\n");
        return 2;
    }
    if (getuid() != 0) {
        fprintf(2, "login: must be run as root\n");
        return 1;
    }

    long uid = -1;
    char expected[65];
    int known = (lookup(argv[1], &uid, expected) == 0);

    /* Ask for the password even if the name is unknown, so that the answer
     * does not reveal which names exist */
    char password[64], actual[65];
    uint8_t digest[32];
    printf("password: ");
    read_password(password, sizeof(password));

    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, (const uint8_t *)password, strlen(password));
    sha256_final(&ctx, digest);
    to_hex(digest, 32, actual);
    memset(password, 0, sizeof(password));     /* Do not leave it lying in memory */

    if (!known || strcmp(actual, expected) != 0) {
        sleep_ms(500);                         /* Make guessing in a loop slow */
        fprintf(2, "login: incorrect name or password\n");
        return 1;
    }

    long err = setuid((unsigned)uid);
    if (err < 0) {
        fprintf(2, "login: %s\n", strerror(err));
        return 1;
    }

    char *args[] = { "/bin/sh", NULL };
    err = execv("/bin/sh", args);
    fprintf(2, "login: /bin/sh: %s\n", strerror(err));
    return 1;
}
