/* stty: show or change the terminal's settings. One of them so far:
 *
 *   stty            say whether tostop is on ("tostop" or "-tostop")
 *   stty tostop     background jobs that write to the terminal are stopped
 *                   (SIGTTOU) until brought to the foreground with fg
 *   stty -tostop    they may write (the default)
 *
 * (Raw mode belongs to the program that asks for it: tty_raw, and it ends
 * with that program.)
 */
#include "ulib.h"

int main(int argc, char **argv) {
    if (argc == 1) {
        long on = tty_tostop(-1);
        if (on < 0) {
            fprintf(2, "stty: %s\n", strerror(on));
            return 1;
        }
        printf("%s\n", on ? "tostop" : "-tostop");
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        long result;
        if (strcmp(argv[i], "tostop") == 0) result = tty_tostop(1);
        else if (strcmp(argv[i], "-tostop") == 0) result = tty_tostop(0);
        else {
            fprintf(2, "usage: stty [tostop | -tostop]\n");
            return 2;
        }
        if (result < 0) {
            fprintf(2, "stty: %s\n", strerror(result));
            return 1;
        }
    }
    return 0;
}
