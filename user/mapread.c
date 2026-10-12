/* mapread: print one byte of a file through a memory mapping.
 *
 *   mapread FILE OFFSET
 *
 * The whole file is mapped, but only the page holding OFFSET is ever
 * touched, so only that page is read from wherever the file lives.
 */
#include "ulib.h"

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(2, "usage: mapread FILE OFFSET\n");
        return 2;
    }
    long offset = atoi(argv[2]);
    long fd = open(argv[1], O_RDONLY);
    if (fd < 0 || offset < 0) {
        fprintf(2, "mapread: %s: %s\n", argv[1], fd < 0 ? strerror(fd) : "bad offset");
        return 1;
    }
    long size = lseek((int)fd, 0, SEEK_END);
    if (size <= offset) {
        fprintf(2, "mapread: the file has only %ld bytes\n", size);
        return 1;
    }
    unsigned char *map = mmap_file((int)fd, (size_t)size, 0, PROT_READ);
    if (!map) {
        fprintf(2, "mapread: cannot map the file\n");
        return 1;
    }
    close((int)fd);             /* The mapping does not need the descriptor */
    printf("byte %ld of %s is %u\n", offset, argv[1], (unsigned)map[offset]);
    return 0;
}
