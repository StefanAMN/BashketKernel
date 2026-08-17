#ifndef KPRINTF_H
#define KPRINTF_H

#include <stdarg.h>
#include <stdbool.h>

#include <stddef.h>

/**
 * @brief Print to the serial port, the screen and the kernel log (dmesg).
 *        A small printf: %d %u %x %p %s %c %%, the l and z size prefixes
 *        (64-bit arguments), and a width with optional zero padding (%08x). Safe from
 *        any CPU and from interrupt handlers; one call's output is not
 *        interleaved with another's.
 */
void kprintf(const char* fmt, ...);

// Format into buf (always NUL-terminated if size > 0, truncated if needed).
// Returns the number of characters stored, not counting the NUL.
size_t ksnprintf(char *buf, size_t size, const char *fmt, ...);

// Drop the kprintf lock unconditionally. Only for fatal paths (panic, CPU
// exception) that must print even if the fault happened inside kprintf.
void kprintf_force_unlock(void);

/**
 * @brief Stop the kernel for good: interrupts off, the other CPUs told to
 *        stop, "KERNEL PANIC" and `msg` printed (whoever held the output
 *        locks), and this CPU halted. For states the kernel cannot go on
 *        from.
 */
void panic(const char* msg) __attribute__((noreturn));

// Set when a CPU stops the kernel for good (a panic or a fatal exception).
// The other CPUs stop at their next timer tick or wait loop instead of
// carrying on with a kernel that has declared itself broken.
extern volatile bool kernel_halted;

/**
 * @brief Set kernel_halted (see above).
 */
void kernel_halt_other_cpus(void);

#endif
