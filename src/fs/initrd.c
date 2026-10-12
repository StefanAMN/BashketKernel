/**
 * @file initrd.c
 * @brief USTAR (tar) initial ramdisk parser
 *
 * The initrd is a tar archive that the bootloader loads into memory next to
 * the kernel. Each entry is a 512-byte header followed by the file contents,
 * padded to a multiple of 512 bytes. File data is not copied: VFS nodes point
 * straight into the archive.
 */

#include "initrd.h"
#include "vfs.h"
#include "kprintf.h"
#include "string_util.h"

#define TAR_BLOCK 512

// Parse an octal number field. Returns false on anything that is not octal
// digits optionally followed by NUL or space padding.
static bool parse_octal(const char *str, size_t max_len, size_t *out) {
    size_t val = 0;
    size_t i = 0;
    for (; i < max_len && str[i] >= '0' && str[i] <= '7'; i++) {
        val = (val * 8) + (size_t)(str[i] - '0');
    }
    if (i == 0) return false;
    if (i < max_len && str[i] != '\0' && str[i] != ' ') return false;
    *out = val;
    return true;
}

// Walk `path` from the root, creating directories for every component except
// the last. Returns the parent directory and points *leaf at the last
// component, or returns NULL for an empty or over-long path.
static vnode_t *make_parents(char *path, char **leaf) {
    vnode_t *dir = vfs_get_root();

    // Skip "./" and leading slashes that tar may put in front
    while (path[0] == '/' || (path[0] == '.' && path[1] == '/')) {
        path += (path[0] == '/') ? 1 : 2;
    }

    // Drop a trailing slash (directory entries are stored as "bin/")
    size_t len = 0;
    while (path[len] != '\0') len++;
    while (len > 0 && path[len - 1] == '/') path[--len] = '\0';
    if (len == 0) return NULL;

    char *component = path;
    for (char *p = path; *p != '\0'; p++) {
        if (*p != '/') continue;

        *p = '\0';
        dir = vfs_create_dir(dir, component); // Returns the existing one if present
        if (!dir) return NULL;
        component = p + 1;
    }

    *leaf = component;
    return dir;
}

bool initrd_mount(const void *tar_data, size_t tar_size) {
    if (!tar_data || tar_size < TAR_BLOCK) return false;

    vfs_init();

    const uint8_t *ptr = (const uint8_t *)tar_data;
    const uint8_t *end = ptr + tar_size;
    size_t file_count = 0;

    while ((size_t)(end - ptr) >= TAR_BLOCK) {
        const tar_header_t *hdr = (const tar_header_t *)ptr;

        // An all-zero block marks the end of the archive
        if (hdr->name[0] == '\0') break;

        if (hdr->magic[0] != 'u' || hdr->magic[1] != 's' || hdr->magic[2] != 't' ||
            hdr->magic[3] != 'a' || hdr->magic[4] != 'r') {
            kprintf("Initrd: entry %zu is not a USTAR header; stopping\n", file_count);
            return false;
        }

        size_t size;
        if (!parse_octal(hdr->size, sizeof(hdr->size) - 1, &size)) {
            kprintf("Initrd: corrupt size field; stopping\n");
            return false;
        }

        // The data must fit in what is left of the archive
        size_t remaining = (size_t)(end - ptr) - TAR_BLOCK;
        if (size > remaining) {
            kprintf("Initrd: entry '%s' claims %zu bytes but only %zu remain; stopping\n",
                    hdr->name, size, remaining);
            return false;
        }

        // The name field is not NUL-terminated when it is exactly 100 bytes
        char path[sizeof(hdr->name) + 1];
        memcpy(path, hdr->name, sizeof(hdr->name));
        path[sizeof(hdr->name)] = '\0';

        char *leaf = NULL;
        vnode_t *parent = make_parents(path, &leaf);
        if (parent) {
            if (hdr->typeflag == '5') {
                vfs_create_dir(parent, leaf);
            } else if (hdr->typeflag == '0' || hdr->typeflag == '\0') {
                vnode_t *node = vfs_create_file(parent, leaf, ptr + TAR_BLOCK, size);
                if (node) {
                    // Read, execute and set-uid bits come from the archive,
                    // but nothing here is writable by anyone but root:
                    // these are the system's programs and configuration
                    size_t tar_mode = 0644;
                    parse_octal(hdr->mode, sizeof(hdr->mode) - 1, &tar_mode);
                    node->mode = (uint16_t)((tar_mode & 04555) | 0200);
                }
                file_count++;
            }
        }

        size_t data_blocks = (size + TAR_BLOCK - 1) / TAR_BLOCK;
        ptr += TAR_BLOCK + data_blocks * TAR_BLOCK;
    }

    kprintf("Initrd: mounted %zu files\n", file_count);
    return true;
}
