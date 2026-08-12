/**
 * @file power.h
 * @brief Shutting the system down: ending every process, leaving the disks
 *        clean, and switching the machine off or restarting it
 */

#ifndef POWER_H
#define POWER_H

#include <stdbool.h>

/**
 * @brief Shut the system down. Never returns.
 *
 * Every user process but the caller is asked to end (SIGTERM) and, a second
 * later, made to (SIGKILL). Their files are closed as they die, and every
 * write reaches the disk as it is made (the sector cache writes through), so
 * once they are gone the ext2 volume is marked clean. Then the machine is
 * switched off through ACPI, or reset. If that fails, a message says it is
 * safe to switch the machine off, and the CPUs halt.
 *
 * @param restart Reset the machine instead of switching it off.
 */
void system_shutdown(bool restart) __attribute__((noreturn));

#endif // POWER_H
