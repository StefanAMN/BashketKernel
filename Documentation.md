# Kernel Development Learning Guide

This document tracks the recommended order of topics and concepts to learn as features are implemented in the kernel repository. It serves as a self-study roadmap for understanding the inner workings of BashketKernel.

---

## Recommended Learning Path

### 1. Booting & Limine Protocol (Phase 1)
**Goal:** Understand how the kernel starts, boots into 64-bit Long Mode, and writes output to the screen.

- **Order of Learning:**
  1. **Limine Protocol & Boot Request Structure:**
     - Learn how requests (e.g. Framebuffer request) are passed between Limine and the kernel using static volatile structures in C.
     - Files to inspect: `src/main.c`, `limine.h`
  2. **VGA/Framebuffer Drawing:**
     - Understand pixel mapping, linear framebuffer addresses, pitch, and color formats.
     - Files to inspect: `src/console.h`, `src/console.c`
  3. **Serial Communications (COM1):**
     - Learn how port I/O (`outb`/`inb`) works in x86 architecture for line-by-line debugging via `0x3F8`.
     - Files to inspect: `src/serial.h`, `src/serial.c`, `src/io.h`
  4. **Formatted Kernel Output (`kprintf`):**
     - Learn how variadic functions (`stdarg.h`) process strings, numbers, hex formatting, and dispatch output to both serial and framebuffer.
     - Files to inspect: `src/kprintf.h`, `src/kprintf.c`

---

### 2. CPU Descriptors, Interrupts & Exceptions (Phase 2)
**Goal:** Understand CPU privilege levels, trap handling, state preservation, and hardware interrupt remapping.

- **Order of Learning:**
  1. **Global Descriptor Table (GDT) & Task State Segment (TSS):**
     - **Concepts:** Flat 64-bit segment model, Access Bytes, Segment Selectors (`0x08` Kernel Code, `0x10` Kernel Data), TSS role in long mode (storing `RSP0` for stack switching).
     - **Assembly reload:** Why `gdt_flush.S` uses `retfq` / far return to reload `CS`.
     - Files to inspect: `src/gdt.h`, `src/gdt.c`, `src/gdt_flush.S`
  2. **Interrupt Descriptor Table (IDT):**
     - **Concepts:** 16-byte gate descriptors in 64-bit mode, interrupt gates vs. trap gates, loading IDT via `lidt`.
     - Files to inspect: `src/idt.h`, `src/idt.c`
  3. **Assembly ISR Stubs & Stack Framing:**
     - **Concepts:** CPU automatic pushes (`SS`, `RSP`, `RFLAGS`, `CS`, `RIP`, + optional error code).
     - **State Preservation:** Pushing all 15 general-purpose registers (`RAX`..`R15`) to form a complete `struct registers` stack layout.
     - **Return:** `iretq` instruction restoring CPU execution state.
     - Files to inspect: `src/isr_stubs.S`, `src/interrupts.h`
  4. **Interrupt Dispatching & PIC Remapping:**
     - **Concepts:** Dispatching from low-level assembly to high-level C `isr_handler`.
     - **Hardware PIC:** Remapping the legacy 8259 dual PIC chips from default vectors (`0x00`-`0x0F`) to (`0x20`-`0x2F` / 32-47) to avoid collision with CPU exception vectors (`0`-`31`).
     - Files to inspect: `src/isr.c`, `src/pic.h`, `src/pic.c`

---

### 3. Memory Management (Phase 3 - Upcoming)
**Goal:** Understand physical frame allocation, virtual page tables, and dynamic kernel heap allocation.

- **Order of Learning (Preview):**
  1. **Limine Memory Map Parsing:** Extracting usable physical memory regions from bootloader memory maps.
  2. **Physical Frame Allocator (Bitmap/Buddy):** Allocating and freeing 4KiB physical frames.
  3. **Virtual Memory Manager (Paging):** 4-level page table walking (PML4 -> PDPT -> PD -> PT), page mapping, and Higher Half Kernel memory layout.
  4. **Kernel Heap Allocator (`kmalloc` / `kfree`):** Managing dynamic kernel memory.

---

### 4. Multitasking & Process Scheduling (Phase 4 - Upcoming)
**Goal:** Task switching, context saving/restoring, kernel threads, and process control blocks (PCBs).

---

### 5. Userspace & System Calls (Phase 5 - Upcoming)
**Goal:** Transitioning to Ring 3, `syscall`/`sysret` handlers, and user stack isolation.
