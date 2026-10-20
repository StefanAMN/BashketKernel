/* basename: the last part of a path name.
 *   basename PATH [SUFFIX]   PATH without what comes before its last '/',
 *                            and without SUFFIX if it ends in it
 * Follows the steps of POSIX: trailing slashes are not a part ("a/b/" is
 * "b"), a path of slashes only is "/", and SUFFIX is left alone when it is
 * the whole of what remains ("basename .c .c" is ".c"). An empty PATH
 * gives an empty line (POSIX allows that or "."). Only the text is looked
 * at: nothing needs to exist.
 */
#include "ulib.h"

int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) {
        fprintf(2, "usage: basename PATH [SUFFIX]\n");
        return 2;
    }
    const char *path = argv[1];
    size_t end = strlen(path);
    while (end > 1 && path[end - 1] == '/') end--;       /* Trailing slashes go (all but one of "///") */
    size_t start = end;
    while (start > 0 && path[start - 1] != '/') start--; /* Back to just after the last '/' */
    if (start == end && end > 0) start = end - 1;          /* Nothing but slashes: keep one */

    if (argc == 3) {
        size_t n = strlen(argv[2]);
        if (n < end - start && memcmp(path + end - n, argv[2], n) == 0) end -= n;
    }
    write(1, path + start, end - start);
    write(1, "\n", 1);
    return 0;
}
