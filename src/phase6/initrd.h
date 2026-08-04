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
 * @brief Parse a USTAR archive image and populate the VFS directory tree.
 * @param tar_data Pointer to start of USTAR archive.
 * @param tar_size Size of USTAR archive in bytes.
 * @return true on successful mount, false on invalid image.
 */
bool initrd_mount(const void *tar_data, size_t tar_size);

#endif // INITRD_H
