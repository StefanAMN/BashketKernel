/* shutdown: stop the system.
 *
 *   shutdown       switch the machine off
 *   shutdown -r    restart it
 *
 * Root only. The kernel does the work (SYS_REBOOT): every process is asked
 * to end and then made to, the disks are left clean, so that the next start
 * needs no check, and the machine is switched off through ACPI or reset.
 */
#include "ulib.h"

int main(int argc, char **argv) {
    int restart = argc == 2 && strcmp(argv[1], "-r") == 0;
    if (argc > 2 || (argc == 2 && !restart)) {
        fprintf(2, "usage: shutdown [-r]\n");
        return 2;
    }
    long err = reboot(restart);
    fprintf(2, "shutdown: %s\n", strerror(err));    /* Reached only if it failed */
    return 1;
}
