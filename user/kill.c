/* kill: end processes.   kill [-SIGNAL] pid...   (default signal: 15) */
#include "ulib.h"

int main(int argc, char **argv) {
    int sig = 15;
    int first = 1;

    if (argc > 1 && argv[1][0] == '-') {
        sig = (int)atoi(argv[1] + 1);
        first = 2;
    }
    if (first >= argc || sig <= 0) {
        fprintf(2, "usage: kill [-SIGNAL] pid...\n");
        return 2;
    }

    int status = 0;
    for (int i = first; i < argc; i++) {
        long err = kill(atoi(argv[i]), sig);
        if (err < 0) {
            fprintf(2, "kill: %s: %s\n", argv[i], strerror(err));
            status = 1;
        }
    }
    return status;
}
