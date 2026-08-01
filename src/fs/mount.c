/**
 * @file mount.c
 * @brief Mount table (see mount.h)
 */

#include "mount.h"
#include "kprintf.h"
#include "kstring.h"
#include "heap.h"
#include "tty.h"

typedef struct {
    const char *path;
    const char *type;
    const char *source;
} mount_entry_t;

static mount_entry_t table[MOUNT_MAX];
static size_t entries = 0;

int mount_register(const char *path, const char *type, const char *source) {
    if (!path || !type || !source || entries >= MOUNT_MAX) return -1;
    for (size_t i = 0; i < entries; i++) {
        if (strcmp(table[i].path, path) == 0) return -1;
    }
    table[entries].path = path;
    table[entries].type = type;
    table[entries].source = source;
    entries++;
    return 0;
}

size_t mount_count(void) {
    return entries;
}

size_t mount_describe(char *buf, size_t size) {
    size_t len = 0;
    if (size > 0) buf[0] = '\0';
    for (size_t i = 0; i < entries && len + 1 < size; i++) {
        len += ksnprintf(buf + len, size - len, "%s %s %s\n", table[i].source, table[i].path, table[i].type);
    }
    return len;
}

void vfs_list_mounts(void) {
    char *text = kmalloc(1024);
    if (!text) return;
    size_t len = mount_describe(text, 1024);
    tty_write(text, len);
    kfree(text);
}
