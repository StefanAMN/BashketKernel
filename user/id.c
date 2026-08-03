/* id: print the user this process runs as. */
#include "ulib.h"

int main(void) {
    long uid = getuid();
    printf("uid %ld%s\n", uid, uid == 0 ? " (root)" : "");
    return 0;
}
