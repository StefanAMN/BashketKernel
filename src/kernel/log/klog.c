/**
 * @file klog.c
 * @brief Implementation of Kernel Ring Buffer Logger
 */

#include "klog.h"
#include "kprintf.h"
#include "string_util.h"

#define KLOG_BUF_SIZE 4096

static char klog_buffer[KLOG_BUF_SIZE];
static size_t klog_pos = 0;

void klog_init(void) {
    memset(klog_buffer, 0, sizeof(klog_buffer));
    klog_pos = 0;
    klog_write("Kernel Log Init: Logging ring buffer online.\n");
}

void klog_write(const char *msg) {
    if (!msg) return;
    size_t len = strlen(msg);
    for (size_t i = 0; i < len; i++) {
        klog_buffer[klog_pos] = msg[i];
        klog_pos = (klog_pos + 1) % KLOG_BUF_SIZE;
    }
}

void klog_print_dmesg(void) {
    kprintf("--- Kernel Log Buffer (dmesg) ---\n");
    kprintf("%s\n", klog_buffer);
}
