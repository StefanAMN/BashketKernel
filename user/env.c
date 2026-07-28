/* env: print the environment, one NAME=value per line. */
#include "ulib.h"

int main(void) {
    for (char **e = environ; e && *e; e++) printf("%s\n", *e);
    return 0;
}
