/* sleep: wait for a number of seconds without using the CPU. */
#include "ulib.h"

int main(int argc, char **argv) {
    if (argc != 2 || atoi(argv[1]) < 0) {
        fprintf(2, "usage: sleep seconds\n");
        return 2;
    }
    sleep_ms((unsigned long)atoi(argv[1]) * 1000);
    return 0;
}
