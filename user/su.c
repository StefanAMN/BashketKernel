/* su: start a shell as another user.
 *
 *   su [NAME]        NAME defaults to root
 *
 * Anyone may run it, and it asks for the *other* user's password (root is
 * not asked). It can check that password and change user because the file
 * /bin/su belongs to root and is set-uid: while it runs, the process has
 * root's rights, whoever started it. Leaving the shell returns to the
 * caller, as before.
 *
 * A set-uid program is trusted with rights its caller does not have, so it
 * does exactly one thing with them and takes nothing from the caller on
 * trust: here, nothing happens before the password has been checked.
 */
#include "ulib.h"
#include "password.h"

int main(int argc, char **argv) {
    const char *name = argc > 1 ? argv[1] : "root";
    if (argc > 2) {
        fprintf(2, "usage: su [NAME]\n");
        return 2;
    }
    if (geteuid() != 0) {
        fprintf(2, "su: not set-uid root: cannot change user\n");
        return 1;
    }

    struct account account;
    int known = (account_by_name(name, &account) == 0);

    if (getuid() != 0) {                       /* The real user: who is asking */
        static const struct account nobody = { .iterations = PASSWORD_ITERATIONS };
        char password[64];
        read_password("password: ", password, sizeof(password));
        int correct = password_matches(known ? &account : &nobody, password);
        memset(password, 0, sizeof(password));
        if (!known || !correct) {
            sleep_ms(500);
            fprintf(2, "su: incorrect name or password\n");
            return 1;
        }
    } else if (!known) {
        fprintf(2, "su: no such user: %s\n", name);
        return 1;
    }

    /* The groups first: once we are no longer root we may not change them */
    unsigned groups[MAX_GROUPS];
    long err = account_groups(account.name, account.gid, groups, MAX_GROUPS);
    if (err >= 0) err = setgroups((unsigned long)err, groups);
    if (err == 0) err = setgid(account.gid);
    if (err == 0) err = setuid(account.uid);   /* As root: real and effective user both change */
    if (err < 0) {
        fprintf(2, "su: %s\n", strerror(err));
        return 1;
    }

    char *args[] = { "/bin/sh", NULL };
    err = execv("/bin/sh", args);
    fprintf(2, "su: /bin/sh: %s\n", strerror(err));
    return 1;
}
