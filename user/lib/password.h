/* Accounts and passwords: what /bin/login, /bin/su and /bin/passwd share.
 *
 * /etc/passwd holds one line per user:
 *
 *     name:uid:gid:iterations:salt:hash
 *
 * The password itself is not stored. `hash` is PBKDF2-HMAC-SHA256 (RFC 8018)
 * of the password, `salt` 16 random bytes, both in hexadecimal:
 *
 *  - A hash can be checked against a guess but not turned back into the
 *    password.
 *  - The salt is different for every account, so two users with the same
 *    password have different hashes, and a table of hashes worked out in
 *    advance is useless.
 *  - PBKDF2 repeats the hash `iterations` times, on purpose: checking one
 *    password takes a moment, and trying a billion takes a billion moments.
 *    (Programs made for this, such as Argon2, also need a lot of memory per
 *    guess, which this does not.)
 *
 * The file is readable by root only: even hashes are better not handed out.
 */
#ifndef USER_PASSWORD_H
#define USER_PASSWORD_H

#include "ulib.h"

#define PASSWORD_FILE "/etc/passwd"
#define GROUP_FILE    "/etc/group"
#define MAX_GROUPS    16        /* Supplementary groups a process may have (the kernel's limit) */
#define PASSWORD_ITERATIONS 4096
#define PASSWORD_MIN_LENGTH 4

struct account {
    char name[32];
    unsigned uid, gid;
    unsigned iterations;
    uint8_t salt[16];
    uint8_t hash[32];
};

/* Find an account by name, or by user id. 0, -ENOENT, or another error.
 * Lines that are not well formed are skipped; only the first 4 KiB of the
 * file is looked at. */
long account_by_name(const char *name, struct account *out);
long account_by_uid(unsigned uid, struct account *out);  /* (As account_by_name) */

/* /etc/group holds one line per group, readable by all:
 *
 *     name:x:gid:member,member,...
 *
 * Fill `groups` (room for `max`) with the gids of the groups that list
 * `name` as a member, leaving out `primary` (the account's own group, which
 * it is in anyway) and repeats. Returns how many; 0 if there is no such
 * file; -E2BIG if there are more than `max`. */
long account_groups(const char *name, unsigned primary, unsigned *groups, int max);

/* PBKDF2-HMAC-SHA256, 32 bytes of output */
void password_hash(const char *password, const uint8_t *salt, size_t salt_len, unsigned iterations, uint8_t out[32]);

/* Does the password belong to the account? */
int password_matches(const struct account *account, const char *password);

/* Give an account a new password (and a new salt). Needs to be root.
 * -EFBIG, changing nothing, if the file is larger than 4 KiB. */
long account_set_password(const char *name, const char *password);

/* Print `prompt` and read a line from the terminal without showing it (raw
 * mode; backspace works). Returns its length (cut to size - 1). */
long read_password(const char *prompt, char *buf, size_t size);

#endif
