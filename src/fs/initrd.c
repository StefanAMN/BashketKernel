/**
 * @file initrd.c
 * @brief Implementation of USTAR Tar Initrd Parser and VFS Mount
 */

#include "initrd.h"
#include "vfs.h"
#include "kprintf.h"
#include "string_util.h"

static size_t parse_octal(const char *str, size_t max_len) {
    size_t val = 0;
    for (size_t i = 0; i < max_len && str[i] != '\0' && str[i] != ' '; i++) {
        if (str[i] >= '0' && str[i] <= '7') {
            val = (val * 8) + (str[i] - '0');
        }
    }
    return val;
}

bool initrd_mount(const void *tar_data, size_t tar_size) {
    if (!tar_data || tar_size < 512) return false;

    vfs_init();
    vnode_t *root = vfs_get_root();

    const uint8_t *ptr = (const uint8_t *)tar_data;
    const uint8_t *end = ptr + tar_size;

    size_t file_count = 0;

    while (ptr + 512 <= end) {
        const tar_header_t *hdr = (const tar_header_t *)ptr;

        // Two consecutive empty blocks mark end of tar archive
        if (hdr->name[0] == '\0') break;

        size_t size = parse_octal(hdr->size, 11);
        const void *file_data = ptr + 512;

        if (hdr->typeflag == '5') {
            // Directory node
            vfs_create_dir(root, hdr->name);
        } else if (hdr->typeflag == '0' || hdr->typeflag == '\0') {
            // Regular file node
            vfs_create_file(root, hdr->name, file_data, size);
            file_count++;
        }

        // USTAR blocks are padded to 512-byte boundaries
        size_t blocks = (size + 511) / 512;
        ptr += 512 + (blocks * 512);
    }

    kprintf("Initrd Mount: Mounted USTAR image (%zu files loaded into VFS)\n", file_count);
    return true;
}
