/**
 * @file procfs.c
 * @brief Implementation of Pseudo-Filesystem (/proc) Nodes
 */

#include "procfs.h"
#include "vfs.h"
#include "kprintf.h"

static const char version_text[] = "BashketOS Kernel 64-bit v1.0.0 (x86_64 Long Mode)\n";
static const char cpuinfo_text[] = "vendor_id\t: GenuineIntel / AuthenticAMD\ncpu family\t: x86_64\nmodel name\t: Generic x86_64 Processor\ncores\t\t: 2\n";

void procfs_init(void) {
    vfs_init();
    vnode_t *root = vfs_get_root();
    vnode_t *proc_dir = vfs_create_dir(root, "proc");

    if (proc_dir) {
        vfs_create_file(proc_dir, "version", version_text, sizeof(version_text) - 1);
        vfs_create_file(proc_dir, "cpuinfo", cpuinfo_text, sizeof(cpuinfo_text) - 1);
        kprintf("ProcFS Init: Mounted pseudo-filesystem at '/proc'\n");
    }
}
