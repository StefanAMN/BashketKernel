/**
 * @file pic.h
 * @brief 8259 Programmable Interrupt Controller (PIC) Interface
 * 
 * By default, the BIOS initializes the master 8259 PIC to use IRQs 0..7 
 * on vectors 0x08..0x0F. However, x86 CPU exceptions also use vectors 0..31.
 * We must remap the 8259 PIC so IRQs 0..15 map to vectors 32..47 (0x20..0x2F).
 */

#ifndef PIC_H
#define PIC_H

#include <stdint.h>

#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1

/**
 * @brief Reinitialise both 8259 PICs so their IRQs arrive on vectors
 *        offset1..offset1+7 and offset2..offset2+7, keeping their masks.
 *        Done even though the I/O APIC replaces them: a stray interrupt from
 *        a PIC must not land on a CPU exception vector.
 * @param offset1 Master PIC base vector offset (usually 32 / 0x20).
 * @param offset2 Slave PIC base vector offset (usually 40 / 0x28).
 */
void pic_remap(int offset1, int offset2);

/**
 * @brief Mask every IRQ on both 8259 PICs, so that only the I/O APIC
 *        delivers device interrupts.
 */
void pic_disable(void);

#endif // PIC_H
