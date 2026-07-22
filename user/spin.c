/* spin: use the CPU forever (or sleep forever with "spin sleep", or read
 * forever with "spin read"). Exists to test kill and Ctrl-C. */
#include "ulib.h"

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "sleep") == 0) {
        for (;;) sleep_ms(1000);
    }
    if (argc > 1 && strcmp(argv[1], "read") == 0) {
        char c;
        for (;;) read(0, &c, 1);
    }

    volatile unsigned long counter = 0;
    for (;;) counter++;
}
