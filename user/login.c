/* login: check a password and start a shell as that user.
 *
 *   login NAME
 *   login           asks for the name first (how init runs it on the console)
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

/* Ask for the user name at the terminal. Returns its length, 0 for an empty
 * line (or end of input, which ends the session: init starts a new prompt). */
static int ask_name(char *name, int size) {
    printf("login: ");
    int length = 0;
    char c;
    while (read(0, &c, 1) == 1 && c != '\n') {
        if (c != '\r' && length < size - 1) name[length++] = c;
    }
    name[length] = '\0';
    return length;
}

/* Copy /etc/motd, the message of the day, to the terminal, if there is one */
static void show_motd(void) {
    long fd = open("/etc/motd", O_RDONLY);
    if (fd < 0) return;
    char text[512];
    long n;
    while ((n = read((int)fd, text, sizeof(text))) > 0) write(1, text, (unsigned long)n);
    close((int)fd);
}

int main(int argc, char **argv) {
    static char typed[64];
    if (argc > 2) {
        fprintf(2, "usage: login [NAME]\n");
        return 2;
    }
    if (geteuid() != 0) {
        fprintf(2, "login: must be run as root\n");
        return 1;
    }
    const char *name = argc == 2 ? argv[1] : typed;
    if (argc == 1 && ask_name(typed, sizeof(typed)) == 0) return 1;

    struct account account;
    int known = (account_by_name(name, &account) == 0);

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

    /* The groups first: once we are no longer root we may not change them */
    unsigned groups[MAX_GROUPS];
    long err = account_groups(account.name, account.gid, groups, MAX_GROUPS);
    if (err >= 0) err = setgroups((unsigned long)err, groups);
    if (err == 0) err = setgid(account.gid);
    if (err == 0) err = setuid(account.uid);
    if (err < 0) {
        fprintf(2, "login: %s\n", strerror(err));
        return 1;
    }

    show_motd();
    char *args[] = { "/bin/sh", NULL };
    err = execv("/bin/sh", args);
    fprintf(2, "login: /bin/sh: %s\n", strerror(err));
    return 1;
}
