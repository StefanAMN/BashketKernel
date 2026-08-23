/* ln: give a file another name.
 *
 *   ln EXISTING NEW       a second name for the same file (a hard link;
 *                         on the ext2 disk only, and not for directories)
 *   ln -s TARGET NEW      a symbolic link: NEW refers to TARGET by name, and
 *                         TARGET may be anything, anywhere, or nothing yet
 */
#include "ulib.h"

int main(int argc, char **argv) {
    int soft = argc == 4 && strcmp(argv[1], "-s") == 0;
    if (argc != 3 && !soft) {
        fprintf(2, "usage: ln EXISTING NEW\n       ln -s TARGET NEW\n");
        return 2;
    }
    const char *from = argv[soft ? 2 : 1], *to = argv[soft ? 3 : 2];
    long err = soft ? symlink(from, to) : link(from, to);
    if (err < 0) {
        fprintf(2, "ln: %s: %s\n", to, strerror(err));
        return 1;
    }
    return 0;
}
