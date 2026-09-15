/* dirname: the directory part of a path name.
 *   dirname PATH       PATH without its last part
 * Follows the steps of POSIX: trailing slashes are not a part ("a/b/" is
 * "a"), a name with no '/' is in "." ("a" and "" give "."), and what
 * would be left empty after a leading '/' is "/" ("/a" and "///" give
 * "/"); the slashes between the two parts go too ("/a//b" is "/a"). Only
 * the text is looked at: nothing needs to exist.
 */
#include "ulib.h"

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(2, "usage: dirname PATH\n");
        return 2;
    }
    const char *path = argv[1];
    size_t end = strlen(path);
    while (end > 0 && path[end - 1] == '/') end--;        /* Trailing slashes */
    while (end > 0 && path[end - 1] != '/') end--;        /* The last part */
    if (end == 0) {
        /* No '/' before the last part: "." for a name, "/" if it was all slashes */
        write(1, path[0] == '/' ? "/\n" : ".\n", 2);
        return 0;
    }
    while (end > 1 && path[end - 1] == '/') end--;        /* The slashes in between */
    write(1, path, end);
    write(1, "\n", 1);
    return 0;
}
