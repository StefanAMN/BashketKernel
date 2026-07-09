#ifndef PIC_H
#define PIC_H

// Remaps the 8259 PIC so hardware IRQs land on vectors 0x20-0x2F instead of
// their BIOS default (0x08-0x0F for the master PIC), which collides with
// CPU exception vectors -- IRQ0 (timer) would otherwise fire on vector 8,
// the same vector as #DF (Double Fault).
//
// Masks every IRQ line after remapping: no IRQ handler exists yet (that's
// PIT/PS2, a later Phase 2 slice), so nothing should currently be able to
// reach the CPU. This intentionally leaves it safe to `sti` later without
// wiring interrupt handlers first.
void pic_remap(void);

#endif // PIC_H
