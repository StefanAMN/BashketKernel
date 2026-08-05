/* orphan: leave a child process behind.
 *
 *   orphan [MILLISECONDS]
 *
 * The parent exits at once; the child lives on for the given time (default
 * 300 ms), then prints who its parent is now and exits. With an init process
 * running, that parent is init, which also collects the child.
 */
#include "ulib.h"

int main(int argc, char **argv) {
    long ms = argc > 1 ? atoi(argv[1]) : 300;
    long first_parent = getpid();

    long pid = fork();
    if (pid < 0) return 1;
    if (pid > 0) return 0;      /* The parent: gone before the child is */

    sleep_ms((unsigned long)ms);
    long parent = getppid();
    printf("orphan %ld: parent was %ld, is now %ld\n", getpid(), first_parent, parent);
    return parent != first_parent ? 0 : 1;
}
