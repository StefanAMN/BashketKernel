/**
 * @file user_apps.c
 * @brief Implementation of Userland Binary Registration
 */

#include "user_apps.h"
#include "vfs.h"
#include "elf.h"
#include "kprintf.h"
#include "string_util.h"

static Elf64_Ehdr hello_elf;
static Elf64_Ehdr calc_elf;

void user_apps_init(void) {
    vfs_init();
    vnode_t *root = vfs_get_root();
    vnode_t *bin_dir = vfs_create_dir(root, "bin");

    // Construct mock ELF header for /bin/hello
    memset(&hello_elf, 0, sizeof(hello_elf));
    hello_elf.e_ident[0] = 0x7F;
    hello_elf.e_ident[1] = 'E';
    hello_elf.e_ident[2] = 'L';
    hello_elf.e_ident[3] = 'F';
    hello_elf.e_ident[4] = 2; // 64-bit
    hello_elf.e_type = 2;     // Executable
    hello_elf.e_entry = 0x400000;

    // Construct mock ELF header for /bin/calc
    memset(&calc_elf, 0, sizeof(calc_elf));
    calc_elf.e_ident[0] = 0x7F;
    calc_elf.e_ident[1] = 'E';
    calc_elf.e_ident[2] = 'L';
    calc_elf.e_ident[3] = 'F';
    calc_elf.e_ident[4] = 2; // 64-bit
    calc_elf.e_type = 2;     // Executable
    calc_elf.e_entry = 0x401000;

    vfs_create_file(bin_dir, "hello", &hello_elf, sizeof(hello_elf));
    vfs_create_file(bin_dir, "calc", &calc_elf, sizeof(calc_elf));

    kprintf("User Apps Init: Installed built-in binaries (/bin/hello, /bin/calc)\n");
}
