/* The first program: proves that code runs in ring 3, that arguments arrive,
 * and that system calls make the round trip. */
#include "ulib.h"

int main(int argc, char **argv) {
    printf("Hello from ring 3! I am pid %d.\n", (int)getpid());
    for (int i = 0; i < argc; i++) {
        printf("  argv[%d] = %s\n", i, argv[i]);
    }
    return 0;
}
