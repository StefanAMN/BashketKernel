/* passwd: change a password.
 *
 *   passwd           your own
 *   passwd NAME      someone else's (root only)
 *
 * The password file can only be written by root, and yet every user must be
 * able to change one line of it. So /bin/passwd is set-uid root: it runs
 * with root's rights, and it is the program, not the kernel, that makes
 * sure those rights are used for the caller's own line and no other. It
 * knows who the caller is from the real user id, which a set-uid program
 * keeps.
 */
#include "ulib.h"
#include "password.h"

int main(int argc, char **argv) {
    if (argc > 2) {
        fprintf(2, "usage: passwd [NAME]\n");
        return 2;
    }
    if (geteuid() != 0) {
        fprintf(2, "passwd: not set-uid root: cannot write the password file\n");
        return 1;
    }

    unsigned caller = (unsigned)getuid();
    struct account account;
    long err = argc == 2 ? account_by_name(argv[1], &account) : account_by_uid(caller, &account);
    if (err < 0) {
        fprintf(2, "passwd: no such user\n");
        return 1;
    }
    if (caller != 0 && account.uid != caller) {
        fprintf(2, "passwd: permission denied: you may only change your own password\n");
        return 1;
    }

    char current[64], first[64], second[64];
    if (caller != 0) {
        /* Someone at an unattended terminal should not be able to do this */
        read_password("current password: ", current, sizeof(current));
        int correct = password_matches(&account, current);
        memset(current, 0, sizeof(current));
        if (!correct) {
            sleep_ms(500);
            fprintf(2, "passwd: incorrect password\n");
            return 1;
        }
    }

    read_password("new password: ", first, sizeof(first));
    read_password("again: ", second, sizeof(second));
    int same = strcmp(first, second) == 0;
    memset(second, 0, sizeof(second));
    if (!same) {
        fprintf(2, "passwd: the two do not match; nothing changed\n");
        return 1;
    }
    if (strlen(first) < PASSWORD_MIN_LENGTH) {
        fprintf(2, "passwd: too short (at least %d characters); nothing changed\n", PASSWORD_MIN_LENGTH);
        return 1;
    }

    err = account_set_password(account.name, first);
    memset(first, 0, sizeof(first));
    if (err < 0) {
        fprintf(2, "passwd: %s\n", strerror(err));
        return 1;
    }
    printf("passwd: password changed for %s\n", account.name);
    return 0;
}
