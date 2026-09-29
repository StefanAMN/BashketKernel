/* diskio: read and write raw sectors of the disk through /dev/vda.
 *
 *   diskio write SECTOR TEXT   store TEXT at the start of a sector
 *   diskio read SECTOR         print the text stored there
 *   diskio size                print the size of the disk
 *
 * What is written survives a reboot: it is on the disk, not in memory.
 * Careful: this bypasses any filesystem on the disk.
 */
#include "ulib.h"

#define SECTOR 512

int main(int argc, char **argv) {
    long fd = open("/dev/vda", O_RDWR);
    if (fd < 0) {
        fprintf(2, "diskio: /dev/vda: %s\n", strerror(fd));
        return 1;
    }

    if (argc == 2 && strcmp(argv[1], "size") == 0) {
        long bytes = lseek((int)fd, 0, SEEK_END);
        printf("%ld sectors (%ld MiB)\n", bytes / SECTOR, bytes / (1024 * 1024));
        return 0;
    }

    if (argc < 3 || atoi(argv[2]) < 0) {
        fprintf(2, "usage: diskio write SECTOR TEXT | read SECTOR | size\n");
        return 2;
    }

    long where = lseek((int)fd, atoi(argv[2]) * SECTOR, SEEK_SET);
    if (where < 0) {
        fprintf(2, "diskio: seek: %s\n", strerror(where));
        return 1;
    }

    static char sector[SECTOR];
    if (strcmp(argv[1], "write") == 0 && argc == 4) {
        size_t len = strlen(argv[3]);
        if (len >= SECTOR) len = SECTOR - 1;
        memcpy(sector, argv[3], len);           /* The rest of the sector is zeros */
        long n = write((int)fd, sector, SECTOR);
        if (n != SECTOR) {
            fprintf(2, "diskio: write: %s\n", n < 0 ? strerror(n) : "short write");
            return 1;
        }
        return 0;
    }

    if (strcmp(argv[1], "read") == 0) {
        long n = read((int)fd, sector, SECTOR);
        if (n != SECTOR) {
            fprintf(2, "diskio: read: %s\n", n < 0 ? strerror(n) : "past the end of the disk");
            return 1;
        }
        sector[SECTOR - 1] = '\0';
        printf("%s\n", sector);
        return 0;
    }

    fprintf(2, "usage: diskio write SECTOR TEXT | read SECTOR | size\n");
    return 2;
}
