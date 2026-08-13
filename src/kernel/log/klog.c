/**
 * @file klog.c
 * @brief Kernel message log (see klog.h)
 */

#include "klog.h"
#include "tty.h"
#include "spinlock.h"

static char ring[KLOG_SIZE];
static size_t total_written = 0; // Bytes ever appended; the newest is at (total_written - 1) % KLOG_SIZE

static spinlock_t klog_lock = SPINLOCK_RANKED("klog", LOCK_RANK_KLOG);

void klog_force_unlock(void) {
    spinlock_force_release(&klog_lock);
}

void klog_putchar(char c) {
    uint64_t flags = spinlock_acquire_irqsave(&klog_lock);
    ring[total_written % KLOG_SIZE] = c;
    total_written++;
    spinlock_release_irqrestore(&klog_lock, flags);
}

void klog_write(const char *msg) {
    if (!msg) return;
    while (*msg) klog_putchar(*msg++);
}

size_t klog_length(void) {
    return total_written < KLOG_SIZE ? total_written : KLOG_SIZE;
}

size_t klog_read(size_t offset, char *buf, size_t count) {
    if (!buf) return 0;

    uint64_t flags = spinlock_acquire_irqsave(&klog_lock);
    size_t held = klog_length();
    size_t oldest = total_written - held; // Position of the oldest byte still held
    size_t n = 0;
    while (n < count && offset + n < held) {
        buf[n] = ring[(oldest + offset + n) % KLOG_SIZE];
        n++;
    }
    spinlock_release_irqrestore(&klog_lock, flags);
    return n;
}

void klog_print_dmesg(void) {
    // Straight to the terminal: printing through kprintf would append the
    // log to itself while it is being read.
    char chunk[128];
    size_t offset = 0, n;
    size_t end = klog_length(); // Stop where the log ended when we started
    while (offset < end && (n = klog_read(offset, chunk, sizeof(chunk))) > 0) {
        if (offset + n > end) n = end - offset;
        tty_write(chunk, n);
        offset += n;
    }
}
