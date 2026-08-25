/**
 * @file klog.h
 * @brief Kernel Ring Buffer Logger & dmesg Diagnostic Interface
 */

#ifndef KLOG_H
#define KLOG_H

#include <stdint.h>
#include <stddef.h>

/**
 * @brief Initialize kernel ring buffer logging system.
 */
void klog_init(void);

/**
 * @brief Write log entry to kernel ring buffer.
 * @param msg Log message string.
 */
void klog_write(const char *msg);

/**
 * @brief Print kernel ring buffer log history to console (dmesg).
 */
void klog_print_dmesg(void);

#endif // KLOG_H
