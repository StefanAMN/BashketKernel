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
     - Files to inspect: `src/phase1/console.h`, `src/phase1/console.c`
  3. **Serial Communications (COM1):**
     - Learn how port I/O (`outb`/`inb`) works in x86 architecture for line-by-line debugging via `0x3F8`.
     - Files to inspect: `src/phase1/serial.h`, `src/phase1/serial.c`, `src/phase1/io.h`
  4. **Formatted Kernel Output (`kprintf`):**
     - Learn how variadic functions (`stdarg.h`) process strings, numbers, hex formatting, and dispatch output to both serial and framebuffer.
     - Files to inspect: `src/phase1/kprintf.h`, `src/phase1/kprintf.c`

---

### 2. CPU Descriptors, Interrupts & Exceptions (Phase 2)
**Goal:** Understand CPU privilege levels, trap handling, state preservation, and hardware interrupt remapping.

- **Order of Learning:**
  1. **Global Descriptor Table (GDT) & Task State Segment (TSS):**
     - **Concepts:** Flat 64-bit segment model, Access Bytes, Segment Selectors (`0x08` Kernel Code, `0x10` Kernel Data), TSS role in long mode (storing `RSP0` for stack switching).
     - **Assembly reload:** Why `gdt_flush.S` uses `retfq` / far return to reload `CS`.
     - Files to inspect: `src/phase2/gdt.h`, `src/phase2/gdt.c`, `src/phase2/gdt_flush.S`
  2. **Interrupt Descriptor Table (IDT):**
     - **Concepts:** 16-byte gate descriptors in 64-bit mode, interrupt gates vs. trap gates, loading IDT via `lidt`.
     - Files to inspect: `src/phase2/idt.h`, `src/phase2/idt.c`
  3. **Assembly ISR Stubs & Stack Framing:**
     - **Concepts:** CPU automatic pushes (`SS`, `RSP`, `RFLAGS`, `CS`, `RIP`, + optional error code).
     - **State Preservation:** Pushing all 15 general-purpose registers (`RAX`..`R15`) to form a complete `struct registers` stack layout.
     - **Return:** `iretq` instruction restoring CPU execution state.
     - Files to inspect: `src/phase2/isr_stubs.S`, `src/phase2/interrupts.h`
  4. **Interrupt Dispatching & PIC Remapping:**
     - **Concepts:** Dispatching from low-level assembly to high-level C `isr_handler`.
     - **Hardware PIC:** Remapping the legacy 8259 dual PIC chips from default vectors (`0x00`-`0x0F`) to (`0x20`-`0x2F` / 32-47) to avoid collision with CPU exception vectors (`0`-`31`).
     - Files to inspect: `src/phase2/isr.c`, `src/phase2/pic.h`, `src/phase2/pic.c`

---

### 3. Memory Management (Phase 3)
**Goal:** Understand physical frame allocation, virtual page tables, and dynamic kernel heap allocation.

- **Order of Learning:**
  1. **Physical Memory Manager (PMM):**
     - **Concepts:** Parsing Limine memory map entries (`USABLE`, `RESERVED`, `KERNEL_AND_MODULES`).
     - **Frame Allocator:** 4KiB page frame bitmap allocation. Using HHDM (`phys + hhdm_offset`) to access physical RAM.
     - Files to inspect: `src/phase3/pmm.h`, `src/phase3/pmm.c`
  2. **Virtual Memory Manager (VMM & 4-Level Paging):**
     - **Concepts:** x86_64 4-level paging structure (PML4 -> PDPT -> PD -> PT).
     - **Page Flags:** `VMM_PRESENT`, `VMM_WRITABLE`, `VMM_USER`, `VMM_NO_EXECUTE`.
     - **Page Mapping & TLB:** Address translation, page table creation, invalidating TLB entries via `invlpg`.
     - Files to inspect: `src/phase3/vmm.h`, `src/phase3/vmm.c`
  3. **Page Fault Exception Diagnostics:**
     - **Concepts:** Vector 14 handling, reading fault address from `CR2`, decoding fault causes (present vs non-present, read vs write, user vs kernel).
     - Files to inspect: `src/phase2/isr.c`
  4. **Kernel Heap Allocator (`kmalloc` / `kfree`):**
     - **Concepts:** Dynamic memory management on virtual address ranges, block headers, first-fit searching, block splitting, and free block coalescing.
     - Files to inspect: `src/phase3/heap.h`, `src/phase3/heap.c`

---

### 4. Multitasking & Preemptive Scheduling (Phase 4)
**Goal:** Task Control Blocks (TCB), stack creation, assembly context switching, 8259 PIT timer driver, and preemptive round-robin scheduling.

- **Order of Learning:**
  1. **Task Control Block (TCB):**
     - **Concepts:** Task states (`READY`, `RUNNING`, `BLOCKED`, `ZOMBIE`), PID allocation, 16 KiB allocated kernel stacks, and per-process page tables (`PML4`).
     - Files to inspect: `src/phase4/task.h`
  2. **Assembly Context Switch (`ctx_switch.S`):**
     - **Concepts:** Saving callee-saved registers (`RBX`, `RBP`, `R12`-`R15`, `RFLAGS`), saving/loading `RSP`, switching `CR3` page table registers, popping saved state, and using `ret` to seamlessly jump into target task entry points.
     - Files to inspect: `src/phase4/ctx_switch.S`
  3. **8254 Programmable Interval Timer (PIT):**
     - **Concepts:** Configuring Channel 0 at 100 Hz (`0x36` command byte to port `0x43`), handling IRQ 0 (Vector 32), and invoking preemptive scheduler ticks.
     - Files to inspect: `src/phase4/pit.h`, `src/phase4/pit.c`
  4. **Round-Robin Scheduler:**
     - **Concepts:** Managing queue of tasks, `sched_yield()`, `sched_tick()`, `task_create()`, and cooperative/preemptive time slicing.
     - Files to inspect: `src/phase4/sched.h`, `src/phase4/sched.c`

---

### 5. Userspace & System Calls (Phase 5)
**Goal:** Fast fast system calls via `syscall`/`sysret` MSR configuration, assembly entry trampolines, and ELF64 binary executable loading.

- **Order of Learning:**
  1. **x86_64 Syscall MSR Configuration:**
     - **Concepts:** Enabling SCE bit in `IA32_EFER` MSR (`0xC0000080`), setting GDT CS/SS selectors in `IA32_STAR` MSR (`0xC0000081`), setting entry handler in `IA32_LSTAR` MSR (`0xC0000082`), and RFLAGS mask in `IA32_SFMASK` (`0xC0000084`).
     - Files to inspect: `src/phase5/syscall.h`, `src/phase5/syscall.c`
  2. **Syscall Assembly Trampoline (`syscall_entry.S`):**
     - **Concepts:** Preserving user state (`RIP` in `RCX`, `RFLAGS` in `R11`), pushing general-purpose registers to form stack frames, invoking C `syscall_handler`, and returning via `sysretq`.
     - Files to inspect: `src/phase5/syscall_entry.S`
  3. **ELF64 Executable Format Loader:**
     - **Concepts:** Validating ELF magic (`0x7F 'E' 'L' 'F'`), parsing `Elf64_Ehdr` and `Elf64_Phdr`, allocating virtual pages for `PT_LOAD` segments, setting page permissions, and returning binary entry point.
     - Files to inspect: `src/phase5/elf.h`, `src/phase5/elf.c`

---

### 6. Virtual Filesystem & Initrd (Phase 6)
**Goal:** Virtual Filesystem (VFS) tree abstractions, directory traversal, file read/write APIs, and USTAR (tar) ramdisk parsing.

- **Order of Learning:**
  1. **Virtual Filesystem (VFS) Abstraction:**
     - **Concepts:** `vnode_t` structures (`VFS_FILE` vs `VFS_DIRECTORY`), parent/child/sibling tree hierarchy, directory creation, path resolution (`vfs_find`), and `vfs_read`.
     - Files to inspect: `src/phase6/vfs.h`, `src/phase6/vfs.c`
  2. **USTAR (Tar) Initrd Parser:**
     - **Concepts:** Reading 512-byte tar headers, octal size conversion, mounting files and directories into VFS tree on boot.
     - Files to inspect: `src/phase6/initrd.h`, `src/phase6/initrd.c`

---

### 7. Core Input & Interactive Kernel Shell (Phase 7)
**Goal:** PS/2 Keyboard hardware interrupts, scancode mapping, circular input buffers, and interactive command-line shell.

- **Order of Learning:**
  1. **PS/2 Keyboard Controller Driver:**
     - **Concepts:** Reading scancodes from port `0x60` on IRQ 1 (Vector 33), Shift key state handling, converting scancodes to ASCII, and buffering keys in a thread-safe circular queue.
     - Files to inspect: `src/phase7/keyboard.h`, `src/phase7/keyboard.c`
  2. **Interactive Kernel Shell:**
     - **Concepts:** Command loop (`shell_run_cmd`), built-in command handlers (`help`, `ls`, `cat`, `echo`, `mem`, `clear`), terminal prompt rendering, and backspace handling.
     - Files to inspect: `src/phase7/shell.h`, `src/phase7/shell.c`
