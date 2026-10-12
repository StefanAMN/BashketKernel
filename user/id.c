/* id: print the user and group this process runs as. */
#include "ulib.h"

int main(void) {
    long uid = getuid(), euid = geteuid(), gid = getgid();
    printf("uid %ld%s\n", uid, uid == 0 ? " (root)" : "");
    printf("gid %ld\n", gid);
    /* Different only inside a set-uid program */
    if (euid != uid) printf("effective uid %ld%s\n", euid, euid == 0 ? " (root)" : "");
    return 0;
}
