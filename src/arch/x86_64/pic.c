/**
 * @file pic.c
 * @brief Implementation of 8259 PIC Vector Remapping
 */

#include "pic.h"
#include "io.h"

void pic_remap(int offset1, int offset2) {
    uint8_t a1, a2;

    // Save existing masks
    a1 = inb(PIC1_DATA);
    a2 = inb(PIC2_DATA);

    // ICW1: Start initialization sequence in cascade mode
    outb(PIC1_COMMAND, 0x11);
    io_wait();
    outb(PIC2_COMMAND, 0x11);
    io_wait();

    // ICW2: Set vector offsets
    outb(PIC1_DATA, offset1); // Master PIC offset
    io_wait();
    outb(PIC2_DATA, offset2); // Slave PIC offset
    io_wait();

    // ICW3: Tell Master PIC that Slave PIC is at IRQ2 (0000 0100 = 0x04)
    outb(PIC1_DATA, 0x04);
    io_wait();
    // Tell Slave PIC its cascade identity (0000 0010 = 0x02)
    outb(PIC2_DATA, 0x02);
    io_wait();

    // ICW4: Set 8086 mode
    outb(PIC1_DATA, 0x01);
    io_wait();
    outb(PIC2_DATA, 0x01);
    io_wait();

    // Restore saved masks
    outb(PIC1_DATA, a1);
    outb(PIC2_DATA, a2);
}

void pic_disable(void) {
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}
