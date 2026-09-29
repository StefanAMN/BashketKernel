/* mapwrite: change a file through a shared memory mapping.
 *
 *   mapwrite FILE OFFSET TEXT
 *
 * The whole file is mapped shared (MAP_SHARED) and TEXT is copied into it
 * at OFFSET, which with TEXT must lie inside the file. There is no msync:
 * the kernel writes the change to the file when the program exits, which
 * is what this shows (make test-home reads it back after a reboot).
 */
#include "ulib.h"

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(2, "usage: mapwrite FILE OFFSET TEXT\n");
        return 2;
    }
    long offset = atoi(argv[2]);
    size_t len = strlen(argv[3]);
    long fd = open(argv[1], O_RDWR);
    if (fd < 0 || offset < 0) {
        fprintf(2, "mapwrite: %s: %s\n", argv[1], fd < 0 ? strerror(fd) : "bad offset");
        return 1;
    }
    long size = lseek((int)fd, 0, SEEK_END);
    if (size < offset + (long)len) {
        fprintf(2, "mapwrite: the file has only %ld bytes\n", size);
        return 1;
    }
    char *map = mmap_file_shared((int)fd, (size_t)size, 0, PROT_READ | PROT_WRITE);
    if (!map) {
        fprintf(2, "mapwrite: cannot map the file\n");
        return 1;
    }
    memcpy(map + offset, argv[3], len);
    return 0;
}
