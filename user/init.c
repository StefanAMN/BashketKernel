/* init: the first process.
 *
 *   init        the system: run /etc/rc, then keep a login prompt on the console
 *   init -s     only the part below that every system needs (the kernel's own
 *               shell has the console: the developer boot entry, and tests)
 *
 * Whatever else it does, init collects orphans: when a process dies, the
 * kernel hands its children to init, and init waits for each of them when it
 * dies in its turn. Every process then has a parent that collects it, and
 * none is left behind as a zombie.
 *
 * The login prompt is what getty does elsewhere: start /bin/login on the
 * console, and when it ends (the user logged out, or gave the wrong
 * password) start another.
 */
#include "ulib.h"

static void child_changed(int sig) {
    (void)sig; /* Being interrupted is the point: it ends the sleep in main */
}

/* Start `argv` as a child in a process group of its own, which the terminal
 * gives its keys to (Ctrl-C then reaches it, not init). Returns its pid, or
 * a negative error. */
static long start(char *const argv[]) {
    long pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        tcsetpgrp(getpid());
        execv(argv[0], argv);
        fprintf(2, "init: %s: cannot run it\n", argv[0]);
        exit(127);
    }
    return pid;
}

/* Run /etc/rc with the shell and wait until it has finished. A system
 * without one simply has nothing to start. */
static void run_rc(void) {
    long fd = open("/etc/rc", O_RDONLY);
    if (fd < 0) return;
    close((int)fd);
    char *argv[] = { "/bin/sh", "/etc/rc", NULL };
    long pid = start(argv);
    int status;
    if (pid > 0) while (waitpid(pid, &status, 0) != pid) { }
}

int main(int argc, char **argv) {
    int sessions = !(argc > 1 && strcmp(argv[1], "-s") == 0);
    signal(SIGINT, SIG_IGN);    /* Ctrl-C is for the programs on the terminal */
    signal(SIGTSTP, SIG_IGN);
    signal(SIGCHLD, child_changed);

    long login = -1;
    if (sessions) run_rc();

    for (;;) {
        if (sessions && login < 0) {
            char *login_argv[] = { "/bin/login", NULL };
            login = start(login_argv);
            if (login < 0) sleep_ms(1000); /* (Out of processes: try again shortly) */
        }

        int status;
        long pid;
        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
            if (pid == login) login = -1; /* The session is over: another prompt */
        }

        /* Nothing more to collect. The kernel sends SIGCHLD when a child
         * dies or a new one is handed over, which cuts this sleep short; the
         * sleep is finite in case that happened just before it began. */
        if (!sessions || login > 0) sleep_ms(1000);
    }
}
