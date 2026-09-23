/**
 * @file klog.h
 * @brief Kernel message log
 *
 * Everything kprintf prints is also kept in a ring buffer, from the first
 * message of the boot. When the buffer is full the oldest text is
 * overwritten. The shell's `dmesg` and the file /proc/kmsg show its contents.
 */

#ifndef KLOG_H
#define KLOG_H

#include <stdint.h>
#include <stddef.h>

#define KLOG_SIZE (64 * 1024)

void klog_init(void);

/**
 * @brief Append one character. Called by kprintf for every character it
 *        prints. Needs no initialisation and takes no lock: callers run with
 *        interrupts off.
 */
void klog_putchar(char c);

/**
 * @brief Append a string.
 */
void klog_write(const char *msg);

/**
 * @brief Number of bytes currently held (at most KLOG_SIZE).
 */
size_t klog_length(void);

/**
 * @brief Copy part of the log, oldest text first.
 * @param offset Position within the bytes currently held.
 * @return Bytes copied (0 at or past the end).
 */
size_t klog_read(size_t offset, char *buf, size_t count);

/**
 * @brief Print the whole log to the console (without logging it again).
 */
void klog_print_dmesg(void);

#endif // KLOG_H
