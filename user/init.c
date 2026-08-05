/* init: the process that outlives the others.
 *
 * When a process dies, its children are handed to init, and init waits for
 * them when they die in their turn: every process then has a parent that
 * collects it, and none is left behind as a zombie. That is all this one
 * does. (On a full Unix it would also start the system's services and the
 * login prompt; here the kernel's own shell still does the starting.)
 */
#include "ulib.h"

static void child_changed(int sig) {
    (void)sig; /* Being interrupted is the point: it ends the sleep below */
}

int main(void) {
    signal(SIGINT, SIG_IGN);    /* Ctrl-C is for the programs on the terminal */
    signal(SIGTSTP, SIG_IGN);
    signal(SIGCHLD, child_changed);

    for (;;) {
        int status;
        while (waitpid(-1, &status, WNOHANG) > 0) { }

        /* Nothing more to collect. The kernel sends SIGCHLD when a child
         * dies or a new one is handed over, which cuts this sleep short; the
         * sleep is finite in case that happened just before it began. */
        sleep_ms(1000);
    }
}
