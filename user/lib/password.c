/* Accounts and passwords (see password.h) */
#include "password.h"
#include "sha256.h"

/* --- HMAC-SHA256 (RFC 2104) and PBKDF2 (RFC 8018) -----------------------------
 *
 * HMAC(key, message) = H((key ^ opad) || H((key ^ ipad) || message)): a hash
 * that only someone with the key can compute. PBKDF2 chains it: the first
 * round hashes the salt, each later one the result of the one before, and
 * the output is all of them XORed together.
 *
 * The two padded-key blocks are the same in every round, so their hash
 * states are worked out once and copied: two compressions per round
 * instead of four.
 */

typedef struct {
    sha256_ctx_t inner, outer; /* Hash states after the padded key */
} hmac_key_t;

/* Work out the two hash states for `key` (hashed first if longer than a
 * block), then wipe the padded copies from the stack */
static void hmac_key(hmac_key_t *k, const uint8_t *key, size_t key_len) {
    uint8_t block[64], pad[64];
    memset(block, 0, sizeof(block));
    if (key_len > 64) {
        sha256_ctx_t ctx;      /* A key longer than a block is hashed first */
        sha256_init(&ctx);
        sha256_update(&ctx, key, key_len);
        sha256_final(&ctx, block);
    } else {
        memcpy(block, key, key_len);
    }

    for (int i = 0; i < 64; i++) pad[i] = block[i] ^ 0x36;
    sha256_init(&k->inner);
    sha256_update(&k->inner, pad, 64);
    for (int i = 0; i < 64; i++) pad[i] = block[i] ^ 0x5C;
    sha256_init(&k->outer);
    sha256_update(&k->outer, pad, 64);
    memset(block, 0, sizeof(block));
    memset(pad, 0, sizeof(pad));
}

/* HMAC of the message `a` followed by `b` (b_len may be 0) under key `k` */
static void hmac(const hmac_key_t *k, const uint8_t *a, size_t a_len, const uint8_t *b, size_t b_len, uint8_t mac[32]) {
    sha256_ctx_t ctx = k->inner;
    sha256_update(&ctx, a, a_len);
    if (b_len) sha256_update(&ctx, b, b_len);
    sha256_final(&ctx, mac);

    ctx = k->outer;
    sha256_update(&ctx, mac, 32);
    sha256_final(&ctx, mac);
}

void password_hash(const char *password, const uint8_t *salt, size_t salt_len, unsigned iterations, uint8_t out[32]) {
    hmac_key_t key;
    hmac_key(&key, (const uint8_t *)password, strlen(password));

    /* One block of output (32 bytes) is all that is wanted: block number 1 */
    static const uint8_t block_number[4] = { 0, 0, 0, 1 };
    uint8_t round[32];
    hmac(&key, salt, salt_len, block_number, 4, round);
    memcpy(out, round, 32);
    for (unsigned i = 1; i < iterations; i++) {
        hmac(&key, round, 32, NULL, 0, round);
        for (int j = 0; j < 32; j++) out[j] ^= round[j];
    }
    memset(&key, 0, sizeof(key));
}

int password_matches(const struct account *account, const char *password) {
    uint8_t actual[32];
    password_hash(password, account->salt, sizeof(account->salt), account->iterations, actual);

    /* Compare without stopping at the first difference: how long it takes
     * must not say how much of a guess was right */
    uint8_t difference = 0;
    for (int i = 0; i < 32; i++) difference |= (uint8_t)(actual[i] ^ account->hash[i]);
    return difference == 0;
}

/* --- The file ------------------------------------------------------------------ */

/* The value of one lower-case hex digit, or -1 */
static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* `count` bytes from 2 * count hex digits; -1 at anything else */
static int from_hex(const char *text, uint8_t *out, size_t count) {
    for (size_t i = 0; i < count; i++) {
        int high = hex_value(text[2 * i]);
        int low = high < 0 ? -1 : hex_value(text[2 * i + 1]);
        if (low < 0) return -1;
        out[i] = (uint8_t)((high << 4) | low);
    }
    return 0;
}

/* `count` bytes as 2 * count lower-case hex digits and a NUL */
static void to_hex(const uint8_t *bytes, size_t count, char *out) {
    for (size_t i = 0; i < count; i++) {
        out[2 * i] = "0123456789abcdef"[bytes[i] >> 4];
        out[2 * i + 1] = "0123456789abcdef"[bytes[i] & 15];
    }
    out[2 * count] = '\0';
}

/* Split "a:b:c" in place; returns the number of fields found (at most max) */
static int split(char *line, char *fields[], int max) {
    int count = 0;
    while (count < max) {
        fields[count++] = line;
        char *colon = strchr(line, ':');
        if (!colon) break;
        *colon = '\0';
        line = colon + 1;
    }
    return count;
}

/* A field that must be a number: all digits, at most nine of them. A uid
 * written as "x" must not be read as 0, which is root. */
static int number(const char *text, unsigned *out) {
    size_t len = strlen(text);
    if (len == 0 || len > 9) return -1;
    for (size_t i = 0; i < len; i++) if (text[i] < '0' || text[i] > '9') return -1;
    *out = (unsigned)atoi(text);
    return 0;
}

/* One line of the file. Returns 0 if it is a well-formed account. */
static int parse_line(char *line, struct account *out) {
    char *f[7];
    if (split(line, f, 7) != 6) return -1;
    if (f[0][0] == '\0' || strlen(f[0]) >= sizeof(out->name)) return -1;
    if (strlen(f[4]) != 32 || strlen(f[5]) != 64) return -1;

    memset(out, 0, sizeof(*out));
    strcpy(out->name, f[0]);
    if (number(f[1], &out->uid) || number(f[2], &out->gid) || number(f[3], &out->iterations)) return -1;
    if (out->iterations == 0 || out->iterations > 1000000) return -1;
    if (from_hex(f[4], out->salt, 16) != 0 || from_hex(f[5], out->hash, 32) != 0) return -1;
    return 0;
}

static char file_text[4096];

/* Read the file into file_text (NUL-terminated). Returns its length, or a
 * negative error. *complete says whether all of it fitted: a larger file
 * is read only in part (lookups see the accounts that fitted). */
static long read_file(int *complete) {
    long fd = open(PASSWORD_FILE, O_RDONLY);
    if (fd < 0) return fd;
    long total = 0, n = 0;
    while (total < (long)sizeof(file_text) - 1 &&
           (n = read((int)fd, file_text + total, sizeof(file_text) - 1 - (size_t)total)) > 0) total += n;
    char more;
    *complete = !(total == (long)sizeof(file_text) - 1 && read((int)fd, &more, 1) > 0);
    close((int)fd);
    file_text[total] = '\0';
    return total;
}

/* Look through the file; `name` NULL means "the account with this uid" */
static long find(const char *name, unsigned uid, struct account *out) {
    int complete;
    long err = read_file(&complete);
    if (err < 0) return err;

    for (char *line = file_text; *line; ) {
        char *end = strchr(line, '\n');
        if (end) *end = '\0';
        struct account account;
        if (parse_line(line, &account) == 0 &&
            (name ? strcmp(account.name, name) == 0 : account.uid == uid)) {
            *out = account;
            return 0;
        }
        if (!end) break;
        line = end + 1;
    }
    return -ENOENT;
}

long account_by_name(const char *name, struct account *out) { return find(name, 0, out); }
long account_by_uid(unsigned uid, struct account *out)      { return find(NULL, uid, out); }

/* Building a line of the file: append text, or ':' and a number */
static size_t put_text(char *out, size_t used, const char *text) {
    size_t len = strlen(text);
    memcpy(out + used, text, len);
    return used + len;
}

/* (See put_text) */
static size_t put_number(char *out, size_t used, unsigned value) {
    char digits[12];
    int n = 0;
    do { digits[n++] = (char)('0' + value % 10); value /= 10; } while (value);
    out[used++] = ':';
    while (n > 0) out[used++] = digits[--n];
    return used;
}

long account_set_password(const char *name, const char *password) {
    struct account account;
    long err = account_by_name(name, &account);
    if (err < 0) return err;

    /* A new salt with every change, from the kernel's random numbers */
    long fd = open("/dev/random", O_RDONLY);
    if (fd < 0) return fd;
    long got = read((int)fd, account.salt, sizeof(account.salt));
    close((int)fd);
    if (got != (long)sizeof(account.salt)) return -EINVAL;
    account.iterations = PASSWORD_ITERATIONS;
    password_hash(password, account.salt, sizeof(account.salt), account.iterations, account.hash);

    /* Write the file again with that one line replaced. (find() cut the
     * text up, so read it afresh.) Only if all of it was read: writing back
     * what fitted would delete every account after it. The rewrite itself
     * is not atomic: a crash between truncating and writing leaves the
     * file empty (rename does not replace, so there is no safer order). */
    static char updated[4096 + 160];
    int complete;
    err = read_file(&complete);
    if (err < 0) return err;
    if (!complete) return -EFBIG;
    size_t name_len = strlen(name), used = 0;
    for (char *line = file_text; *line; ) {
        char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        if (len > name_len && strncmp(line, name, name_len) == 0 && line[name_len] == ':') {
            char salt_hex[33], hash_hex[65];
            to_hex(account.salt, 16, salt_hex);
            to_hex(account.hash, 32, hash_hex);
            used = put_text(updated, used, account.name);
            used = put_number(updated, used, account.uid);
            used = put_number(updated, used, account.gid);
            used = put_number(updated, used, account.iterations);
            updated[used++] = ':';
            used = put_text(updated, used, salt_hex);
            updated[used++] = ':';
            used = put_text(updated, used, hash_hex);
            updated[used++] = '\n';
        } else if (used + len + 1 < sizeof(updated)) {
            memcpy(updated + used, line, len);
            used += len;
            updated[used++] = '\n';
        }
        if (!end) break;
        line = end + 1;
    }

    fd = open(PASSWORD_FILE, O_WRONLY | O_TRUNC);
    if (fd < 0) return fd;
    long written = write((int)fd, updated, used);
    close((int)fd);
    return written == (long)used ? 0 : -EINVAL;
}

long read_password(const char *prompt, char *buf, size_t size) {
    size_t len = 0;
    if (size == 0) return -EINVAL;
    printf("%s", prompt);
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
