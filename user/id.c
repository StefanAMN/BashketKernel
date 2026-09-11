/* id: print the user and group this process runs as, and the other groups
 * it is in (set at login from /etc/group). */
#include "ulib.h"

int main(void) {
    long uid = getuid(), euid = geteuid(), gid = getgid();
    printf("uid %ld%s\n", uid, uid == 0 ? " (root)" : "");
    printf("gid %ld\n", gid);
    unsigned groups[16];
    long count = getgroups(16, groups);
    if (count > 0) {
        printf("groups");
        for (long i = 0; i < count; i++) printf(" %u", groups[i]);
        printf("\n");
    }
    /* Different only inside a set-uid program */
    if (euid != uid) printf("effective uid %ld%s\n", euid, euid == 0 ? " (root)" : "");
    return 0;
}
