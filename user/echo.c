/* echo: print the arguments separated by spaces. */
#include "ulib.h"

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        printf(i + 1 < argc ? "%s " : "%s", argv[i]);
    }
    printf("\n");
    return 0;
}
