/* login: check a password and start a shell as that user.
 *
 *   login NAME
 *
 * Must be run as root: it reads the password file, which only root can, and
 * changes user id, which only root may. After the switch the shell (and
 * everything it starts) has that user's rights and no way back to root;
 * leaving the shell returns to whoever ran login.
 *
 * How passwords are stored and checked is in user/lib/password.h.
 */
#include "ulib.h"
#include "password.h"

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(2, "usage: login NAME\n");
        return 2;
    }
    if (geteuid() != 0) {
        fprintf(2, "login: must be run as root\n");
        return 1;
    }

    struct account account;
    int known = (account_by_name(argv[1], &account) == 0);

    /* Ask for the password even if the name is unknown, and do the same
     * amount of work with it, so that neither the question nor the time it
     * takes reveals which names exist */
    static const struct account nobody = { .iterations = PASSWORD_ITERATIONS };
    char password[64];
    read_password("password: ", password, sizeof(password));
    int correct = password_matches(known ? &account : &nobody, password);
    memset(password, 0, sizeof(password));     /* Do not leave it lying in memory */

    if (!known || !correct) {
        sleep_ms(500);                         /* Make guessing in a loop slow */
        fprintf(2, "login: incorrect name or password\n");
        return 1;
    }

    /* The group first: once we are no longer root we may not change it */
    long err = setgid(account.gid);
    if (err == 0) err = setuid(account.uid);
    if (err < 0) {
        fprintf(2, "login: %s\n", strerror(err));
        return 1;
    }

    char *args[] = { "/bin/sh", NULL };
    err = execv("/bin/sh", args);
    fprintf(2, "login: /bin/sh: %s\n", strerror(err));
    return 1;
}
