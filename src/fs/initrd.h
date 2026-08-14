/**
 * @file initrd.h
 * @brief USTAR (Tar Archive) Initrd Parser Interface
 */

#ifndef INITRD_H
#define INITRD_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
} __attribute__((packed)) tar_header_t;

/**
 * @brief Add the files and directories of a USTAR archive to the in-memory
 *        tree under the root (the rescue system). File data is not copied:
 *        the nodes point into the archive, which must stay in memory for
 *        good. Modes come from the archive, made writable by root only;
 *        entries other than files and directories are skipped. Every header
 *        is checked against the archive's size.
 * @param tar_data Pointer to start of USTAR archive.
 * @param tar_size Size of USTAR archive in bytes.
 * @return false (having perhaps added some entries) at the first header
 *         that is not USTAR or claims more data than remains.
 */
bool initrd_mount(const void *tar_data, size_t tar_size);

#endif // INITRD_H
