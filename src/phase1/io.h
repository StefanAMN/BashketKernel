/**
 * @file io.h
 * @brief Low-level x86_64 Port I/O Instructions
 * 
 * In x86 architecture, peripheral devices (like the serial port controller, 
 * 8259 PIC, and PS/2 keyboard controller) are accessed through a separate 
 * 16-bit I/O address space (ports 0x0000 to 0xFFFF) using specialized CPU 
 * instructions: IN and OUT.
 */

#ifndef IO_H
#define IO_H

#include <stdint.h>

/**
 * @brief Output a single byte (8 bits) to an I/O port.
 * @param port The 16-bit I/O port address.
 * @param val The 8-bit value to write.
 * 
 * Uses inline assembly with Intel syntax (`out dx, al`) to send a byte to the hardware port.
 */
static inline void outb(uint16_t port, uint8_t val) {
    asm volatile ("out %0, %1" : : "dN"(port), "a"(val));
}

/**
 * @brief Read a single byte (8 bits) from an I/O port.
 * @param port The 16-bit I/O port address.
 * @return The 8-bit value read from the hardware port.
 * 
 * Uses inline assembly (`in al, dx`) to retrieve a byte from the specified I/O port.
 */
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ("in %0, %1" : "=a"(ret) : "dN"(port));
    return ret;
}

/**
 * @brief Brief delay for legacy I/O operations.
 * 
 * Writes a dummy byte to port 0x80 (unused by hardware, traditionally used by POST cards).
 * This introduces a short delay (~1 microsecond) required when communicating with 
 * older slow legacy hardware chips like the 8259 PIC.
 */
static inline void io_wait(void) {
    outb(0x80, 0);
}

#endif // IO_H
