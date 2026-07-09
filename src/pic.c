#include "pic.h"
#include "io.h"

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

#define PIC1_VECTOR_OFFSET 0x20 // master: IRQ0-7  -> vectors 0x20-0x27
#define PIC2_VECTOR_OFFSET 0x28 // slave:  IRQ8-15 -> vectors 0x28-0x2F

#define ICW1_INIT_ICW4 0x11 // ICW1: start init sequence, ICW4 will be sent
#define ICW4_8086_MODE 0x01 // ICW4: 8086/88 mode

void pic_remap(void) {
    // ICW1: begin initialization on both PICs.
    outb(PIC1_CMD, ICW1_INIT_ICW4); io_wait();
    outb(PIC2_CMD, ICW1_INIT_ICW4); io_wait();

    // ICW2: vector offset for each PIC.
    outb(PIC1_DATA, PIC1_VECTOR_OFFSET); io_wait();
    outb(PIC2_DATA, PIC2_VECTOR_OFFSET); io_wait();

    // ICW3: wiring between master and slave. Master gets a bitmask of
    // which IR line the slave is attached to (IRQ2 -> bit 2 -> 0x04);
    // slave gets its own cascade identity (2, matching that same line).
    outb(PIC1_DATA, 0x04); io_wait();
    outb(PIC2_DATA, 0x02); io_wait();

    // ICW4: 8086 mode.
    outb(PIC1_DATA, ICW4_8086_MODE); io_wait();
    outb(PIC2_DATA, ICW4_8086_MODE); io_wait();

    // Mask every line: no IRQ handler exists yet for anything the PIC
    // could deliver, so nothing should be able to reach the CPU until a
    // later slice unmasks the specific lines it wires up (PIT, PS/2).
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}
