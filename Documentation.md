# BashketKernel — Curriculum & Order of Learning

Welcome to **BashketKernel**! This document serves as an ordered, structured guide for learning the concepts and features implemented throughout the 38 phases of the kernel development roadmap.

Each section breaks down a kernel subsystem, key operating system concepts, and the source files for it.

**Read the status line under each heading first.** Where a phase is marked
*partial* or *stub*, the "Concepts" below it describe what the subsystem is
meant to teach, not what the listed files currently contain.

---

### 1. Boot to Output & Serial Infrastructure (Phase 1)
**Status: real.** Framebuffer console, COM1 serial, `kprintf`.

**Goal:** Higher-half kernel entry, Limine boot protocol, text console blitter, COM1 serial logging.

- **Order of Learning:**
  1. **Framebuffer Console Blitter & RAM Backbuffer:**
     - **Concepts:** Linear framebuffers, bitmap font glyph blitting (`BlitChar`), fast RAM double buffering (`console_enable_backbuffer`), zero-MMIO-read 64-bit streaming text row scrolling.
     - Files to inspect: `src/drivers/console/console.h`, `src/drivers/console/console.c`
  2. **Serial Port Logging:**
     - **Concepts:** COM1 UART I/O ports (`0x3F8`), line control register, baud rate divisor.
     - Files to inspect: `src/drivers/char/serial.h`, `src/drivers/char/serial.c`, `src/arch/x86_64/io.h`
  3. **Kernel Printf:**
     - **Concepts:** Standard string formatting (`%s`, `%d`, `%x`, `%p`), varargs (`va_list`).
     - Files to inspect: `src/lib/kprintf.h`, `src/lib/kprintf.c`

---

### 2. CPU Tables & Interrupt Handling (Phase 2)
**Status: real.** Double faults run on their own stack (IST1).

**Goal:** Global Descriptor Table (GDT), Interrupt Descriptor Table (IDT), 8259 PIC remap, ISR stubs.

- **Order of Learning:**
  1. **Global Descriptor Table (GDT):**
     - **Concepts:** Ring 0 and Ring 3 code/data segment descriptors, TSS descriptor.
     - Files to inspect: `src/arch/x86_64/gdt.h`, `src/arch/x86_64/gdt.c`, `src/arch/x86_64/gdt_flush.S`
  2. **Interrupt Descriptor Table (IDT):**
     - **Concepts:** Interrupt gates, trap gates, exception vector routing (0–31).
     - Files to inspect: `src/arch/x86_64/idt.h`, `src/arch/x86_64/idt.c`
  3. **Interrupt Service Routines (ISR):**
     - **Concepts:** CPU stack frame pushing (`rip`, `cs`, `rflags`, `rsp`, `ss`), `iretq`.
     - Files to inspect: `src/arch/x86_64/isr_stubs.S`, `src/arch/x86_64/interrupts.h`
  4. **8259 Programmable Interrupt Controller (PIC):**
     - **Concepts:** Remapping IRQ 0–15 off default CPU exception vectors to `0x20–0x2F`.
     - Files to inspect: `src/arch/x86_64/isr.c`, `src/arch/x86_64/pic.h`, `src/arch/x86_64/pic.c`

---

### 3. Memory Management: PMM, VMM & Kernel Heap (Phase 3)
**Status: real.** Bitmap frame allocator, 4-level paging, first-fit heap. One address space only.

**Goal:** Physical frame bitmap allocator, 4-level x86_64 page tables, kernel heap allocator.

- **Order of Learning:**
  1. **Physical Memory Manager (PMM):**
     - **Concepts:** Frame allocation, 4 KiB page aligned bitmap tracking, Limine memory map parsing.
     - Files to inspect: `src/mm/pmm.h`, `src/mm/pmm.c`
  2. **Virtual Memory Manager (VMM):**
     - **Concepts:** x86_64 4-level paging (PML4, PDPT, PD, PT), Higher-Half Direct Map (HHDM).
     - Files to inspect: `src/mm/vmm.h`, `src/mm/vmm.c`
  3. **Page Fault Handler:**
     - **Concepts:** CR2 register inspection, page fault error codes (Present, Write, User).
     - Files to inspect: `src/arch/x86_64/isr.c`
  4. **Kernel Heap Allocator:**
     - **Concepts:** Free-list memory allocation (`kmalloc`, `kfree`), block splitting and coalescing.
     - Files to inspect: `src/mm/heap.h`, `src/mm/heap.c`

---

### 4. Multitasking & Context Switching (Phase 4)
**Status: real.** Preemptive round-robin kernel threads, guarded kernel stacks, zombie reaping. Tasks block (sleep, input, wait, semaphores) instead of polling. 100 Hz PIT tick.

**Goal:** Task Control Block (TCB), cooperative and preemptive round-robin scheduler.

- **Order of Learning:**
  1. **Task Control Block & Task Creation:**
     - **Concepts:** Task states (READY, RUNNING, SLEEPING, DEAD), System V AMD64 ABI 16-byte stack frame alignment (`(RSP + 8) % 16 == 0` at entry), saved register context.
     - Files to inspect: `src/kernel/sched/task.h`, `src/kernel/sched/sched.c`
  2. **Context Switching:**
     - **Concepts:** Callee-saved register saving/restoring (`rbx`, `rbp`, `r12–r15`), `rsp` swapping, address space (`cr3`) switching.
     - Files to inspect: `src/arch/x86_64/ctx_switch.S`
  3. **Timer IRQ (PIT) & Interrupt Acknowledgment:**
     - **Concepts:** 8254 PIT tick frequency configuration, Local APIC and PIC EOI ordering before preemptive context switch, preventing interrupt starvation.
     - Files to inspect: `src/kernel/time/pit.h`, `src/kernel/time/pit.c`, `src/arch/x86_64/isr.c`
  4. **Round-Robin Scheduler:**
     - **Concepts:** Runqueue traversal, preemptive scheduling on timer interrupt.
     - Files to inspect: `src/kernel/sched/sched.h`, `src/kernel/sched/sched.c`

---

### 5. Userspace & System Calls (Phase 5)
**Status: real.** Programs run in ring 3 with their own address space. `syscall`/`sysret` entry, validated user pointers, hardened ELF64 loader, per-process file descriptors.

**Goal:** Syscall/sysret MSR setup, user privilege transition (Ring 3), ELF64 loader.

- **Order of Learning:**
  1. **System Call Entry & Exit:**
     - **Concepts:** `STAR`, `LSTAR`, `SFMASK` MSR setup, `syscall` assembly trampoline.
     - Files to inspect: `src/kernel/syscall/syscall.h`, `src/kernel/syscall/syscall.c`
     - Trampoline assembly: `src/arch/x86_64/syscall_entry.S`
  2. **ELF64 Executable Loader:**
     - **Concepts:** Parsing ELF headers and program headers (`PT_LOAD`), segment virtual mapping.
     - Files to inspect: `src/fs/elf.h`, `src/fs/elf.c`

---

### 6. Virtual Filesystem (VFS) & InitRD (Phase 6)
**Status: real.** In-memory VFS tree, readable and writable (contents live in RAM and are lost at power-off). A USTAR initrd with the user programs is loaded as a Limine module.

**Goal:** VFS inode and file operation abstractions, USTAR initrd parser.

- **Order of Learning:**
  1. **Virtual Filesystem (VFS):**
     - **Concepts:** Inodes, directory entries, `vfs_open`, `vfs_read`, `vfs_write`, file descriptors.
     - Files to inspect: `src/fs/vfs.h`, `src/fs/vfs.c`
  2. **Initial RAM Disk (InitRD):**
     - **Concepts:** Parsing USTAR tar format archives loaded as Limine boot modules.
     - Files to inspect: `src/fs/initrd.h`, `src/fs/initrd.c`

---

### 7. Interactive Shell & Keyboard Input (Phase 7)
**Status: real.** Two shells. The boot shell is a kernel thread with debugging commands. `/bin/sh` is a user program with pipelines and redirection, built only on system calls.

**Goal:** PS/2 keyboard scancode decoding, interactive shell commands (`ls`, `cat`, `echo`).

- **Order of Learning:**
  1. **PS/2 Keyboard Driver:**
     - **Concepts:** Scancode Set 1 decoding, keypress modifier state tracking.
     - Files to inspect: `src/drivers/input/keyboard.h`, `src/drivers/input/keyboard.c`
  2. **Interactive Kernel Shell:**
     - **Concepts:** Line buffering, command tokenization, built-in shell commands.
     - Files to inspect: `src/kernel/proc/shell.h`, `src/kernel/proc/shell.c`

---

### 8. Advanced Memory Management (Phase 8)
**Status: partial.** The slab allocator is used for open-file objects; `mmap`/`brk` work per process; `fork` shares pages copy-on-write (`vmm_clone_user_space`, `vmm_handle_cow_fault`). The user stack grows on demand (`process_grow_stack`).

**Goal:** Slab object allocator, Copy-On-Write (COW), Anonymous memory mapping (`mmap`).

- **Order of Learning:**
  1. **Slab Allocator:**
     - **Concepts:** Object caching, fixed-size cache pools, memory fragmentation reduction.
     - Files to inspect: `src/mm/slab.h`, `src/mm/slab.c`
  2. **Copy-On-Write (COW):**
     - **Concepts:** Shared physical pages marked read-only, duplicating pages on write page faults.
     - Files to inspect: `src/mm/cow.h`, `src/mm/cow.c`
  3. **Memory Mapping (mmap):**
     - **Concepts:** Virtual memory region management, `mmap` / `munmap` syscall support.
     - Files to inspect: `src/mm/mmap.h`, `src/mm/mmap.c`

---

### 9. APIC & IOAPIC Interrupt Controller Subsystem (Phase 9)
**Status: real.** ISA IRQs routed from the ACPI MADT. The LAPIC timer is not used.

**Goal:** Local APIC timer, IOAPIC redirection table configuration.

- **Order of Learning:**
  1. **Local APIC (LAPIC):**
     - **Concepts:** Memory-mapped APIC registers, LAPIC timer calibration, EOI signals.
     - Files to inspect: `src/arch/x86_64/lapic.h`, `src/arch/x86_64/lapic.c`
  2. **IOAPIC:**
     - **Concepts:** Replacing legacy 8259 PIC, routing hardware IRQs to specific APIC IDs.
     - Files to inspect: `src/arch/x86_64/ioapic.h`, `src/arch/x86_64/ioapic.c`

---

### 10. SMP Multiprocessing & Spinlocks (Phase 10)
**Status: stub.** Spinlocks are real. Only the boot CPU is ever started.

**Goal:** Multi-core CPU initialization via Limine SMP, kernel spinlocks.

- **Order of Learning:**
  1. **Spinlock Synchronization:**
     - **Concepts:** Atomic exchange (`__sync_lock_test_and_set`), interrupt disabling during lock held.
     - Files to inspect: `src/kernel/sync/spinlock.h`, `src/kernel/sync/spinlock.c`
  2. **Symmetric Multiprocessing (SMP):**
     - **Concepts:** Application Processor (AP) boot bootstrap, per-CPU data structures.
     - Files to inspect: `src/arch/x86_64/smp.h`, `src/arch/x86_64/smp.c`

---

### 11. Storage Subsystem & FAT32 Filesystem (Phase 11)
**Status: real.** virtio-blk is a real driver (legacy PCI interface, one virtqueue, interrupt-driven, one request at a time); `/dev/vda` is the raw disk. The FAT32 volume on it is mounted at `/disk`, readable and writable: files and directories can be created, written, appended, truncated and removed, programs can be run from it, and everything survives a reboot. Limits: 8.3 names only (no long names, no mixed case), no rename, no journal, and the directory tree is read once at boot. See `src/drivers/block/virtio_blk.c`, `src/fs/fat32.c`, `tools/mkfat.py`.

**Goal:** VirtIO Block device driver, FAT32 directory and file reading/writing.

- **Order of Learning:**
  1. **VirtIO Block Storage Driver:**
     - **Concepts:** Virtqueue descriptor rings, avail/used rings, virtio block read/write requests.
     - Files to inspect: `src/drivers/block/virtio_blk.h`, `src/drivers/block/virtio_blk.c`
  2. **FAT32 Filesystem Driver:**
     - **Concepts:** Boot sector parsing, FAT cluster chain traversal, directory entry parsing.
     - Files to inspect: `src/fs/fat32.h`, `src/fs/fat32.c`

---

### 12. Process Life Cycle & Signal Handling (Phase 12)
**Status: real.** `fork` (copy-on-write), `execve`, `waitpid`, `kill`, exit statuses. A faulting process is killed without affecting the kernel. Signals can end a process, be ignored, or run a handler in the program (`signal`), with the interrupted state saved on the user stack and restored by `sigreturn`. They take effect at safe points; a process asleep in a system call is woken, and for a handler the call returns `EINTR`. There is no signal masking and no automatic restart of an interrupted call. See `src/kernel/proc/signal.c`.

**Goal:** Process hierarchy (`fork`, `exec`, `waitpid`, `exit`), UNIX signal dispatching.

- **Order of Learning:**
  1. **Process Management:**
     - **Concepts:** Process Control Block (PCB), PID allocation, parent-child process tree.
     - Files to inspect: `src/kernel/proc/process.h`, `src/kernel/proc/process.c`
  2. **Signal Handling:**
     - **Concepts:** Asynchronous signals (`SIGINT`, `SIGKILL`, `SIGSEGV`), default action vs custom handlers.
     - Files to inspect: `src/kernel/proc/process.h`, `src/kernel/proc/process.c`

---

### 13. PCI Bus & VirtIO Network Driver (Phase 13)
**Status: real.** PCI scan (function 0 of each device), configuration space access, shared interrupt lines. virtio-net is a real driver: receive and transmit virtqueues, interrupt-driven, MAC address from the device.

**Goal:** PCI bus enumeration, VirtIO Net driver (Ethernet frame transmit/receive).

- **Order of Learning:**
  1. **PCI Bus Subsystem:**
     - **Concepts:** Scanning PCI buses (0–255), reading vendor/device IDs via config ports `0xCF8/0xCFC`.
     - Files to inspect: `src/drivers/bus/pci.h`, `src/drivers/bus/pci.c`
  2. **VirtIO Net Interface Card (NIC):**
     - **Concepts:** Rx/Tx virtqueue initialization, MAC address retrieval, raw Ethernet frame transmission.
     - Files to inspect: `src/drivers/net/virtio_net.h`, `src/drivers/net/virtio_net.c`

---

### 14. Bootloader Information Handover (Phase 14)
**Status: stub.** A CPUID check. Limine boots the kernel.

**Goal:** Structured inspection of Limine boot tags (framebuffer, memory map, RSDP, modules).

- **Order of Learning:**
  1. **Boot Tag Inspection:**
     - **Concepts:** Retrieving system boot information handed over from Limine protocol.
     - Files to inspect: `src/boot/boot_loader.h`, `src/boot/boot_loader.c`

---

### 15. PS/2 Mouse Driver & Graphical Cursor (Phase 15)
**Status: stub.** The IRQ handler discards mouse bytes. Nothing is drawn.

**Goal:** PS/2 mouse packet parsing, hardware cursor blitting.

- **Order of Learning:**
  1. **PS/2 Mouse Driver:**
     - **Concepts:** 3-byte mouse packet stream parsing (X/Y delta, left/right/middle button states).
     - Files to inspect: `src/drivers/input/mouse.h`, `src/drivers/input/mouse.c`
  2. **Graphical Cursor Blitter:**
     - **Concepts:** Alpha-blended cursor bitmap overlay, double buffering back-buffer updates.
     - Files to inspect: `src/drivers/input/cursor.h`, `src/drivers/input/cursor.c`

---

### 16. Userland Application Infrastructure (Phase 16)
**Status: real.** `user/*.c` are built into `/bin` with a small C library (`user/lib`): `sh`, `ls`, `cat`, `echo`, `wc`, `hello`, `fault`, `syscheck`.

**Goal:** Loading and executing standalone ring-3 binaries from filesystem.

- **Order of Learning:**
  1. **Userland Execution Manager:**
     - **Concepts:** Spawning ring-3 tasks from ELF binaries located on disk or initrd.
     - Files to inspect: `src/kernel/proc/user_apps.h`, `src/kernel/proc/user_apps.c`

---

### 17. Real-Time Clock (RTC) Driver (Phase 17)
**Status: real.** Reads date and time from CMOS.

**Goal:** Reading CMOS Real-Time Clock registers for system date and time.

- **Order of Learning:**
  1. **CMOS Real-Time Clock:**
     - **Concepts:** Reading CMOS ports `0x70/0x71`, BCD format decoding to UTC time struct.
     - Files to inspect: `src/drivers/char/rtc.h`, `src/drivers/char/rtc.c`

---

### 18. Environment Variables (Phase 18)
**Status: real.** Each process has its own environment in its own memory, carried across `fork` and `execve`. The C library has `getenv`/`setenv`; `/bin/sh` has `export`, `$NAME`, `$?` and `PATH` lookup. (The kernel shell still has a separate global table for its `env`/`export` commands.)

**Goal:** Per-process environment variable management (`getenv`, `setenv`, `env`).

- **Order of Learning:**
  1. **Process Environment Storage:**
     - **Concepts:** Key-value string array storage, inheritance across `fork`/`exec`.
     - Files to inspect: `src/kernel/proc/env.h`, `src/kernel/proc/env.c`

---

### 19. Inter-Process Communication Pipes (Phase 19)
**Status: real.** Blocking pipes behind file descriptors, with end-of-file and broken-pipe handling. Used by `/bin/sh` for `a | b`.

**Goal:** POSIX Anonymous Pipes (`pipe`), blocking read/write ring buffers.

- **Order of Learning:**
  1. **IPC Ring-Buffer Pipes:**
     - **Concepts:** Circular byte buffer, task sleeping on empty/full buffer, pipe EOF handling.
     - Files to inspect: `src/fs/pipe.h`, `src/fs/pipe.c`

---

### 20. Terminal ANSI Escape Sequence Parser (Phase 20)
**Status: real.** The console interprets ANSI escape sequences: 16 text and background colours, bold, cursor positioning and relative movement, erasing the screen or a line. `/bin/clear` and `/bin/colors` use them. Unsupported sequences are swallowed. See `src/gui/compositor/ansi.c` (parser) and `src/drivers/console/console.c` (what the screen does with it).

**Goal:** ANSI escape sequence handling for text coloring and cursor movement.

- **Order of Learning:**
  1. **ANSI Sequence Parser:**
     - **Concepts:** State machine parsing for CSI sequences (e.g. `\033[31m` for red text).
     - Files to inspect: `src/gui/compositor/ansi.h`, `src/gui/compositor/ansi.c`

---

### 21. In-Memory RAM Filesystem (RAMFS) (Phase 21)
**Status: stub.** Creates `/tmp` in the VFS tree. Not a separate filesystem.

**Goal:** Dynamic in-memory hierarchical filesystem (`mkdir`, `create`, `write`, `read`).

- **Order of Learning:**
  1. **RAMFS Storage Engine:**
     - **Concepts:** Dynamic memory inode trees, directory child node linked lists.
     - Files to inspect: `src/fs/ramfs.h`, `src/fs/ramfs.c`

---

### 22. Multi-Level Priority Scheduler (Phase 22)
**Status: real.** Round robin with weighted time slices: high, normal and low priority tasks run for 4, 2 and 1 ticks per turn, so priority sets the CPU share and nothing is starved. Per-task CPU ticks are in `/proc/tasks`.

**Goal:** Priority-based task scheduling (Real-Time, High, Normal, Low priority queues).

- **Order of Learning:**
  1. **Priority Scheduling Engine:**
     - **Concepts:** Multi-level feedback runqueues, priority aging to prevent starvation.
     - Files to inspect: `src/kernel/sched/priority.h`, `src/kernel/sched/priority.c`

---

### 23. Counting Semaphores (Phase 23)
**Status: real.** Waiters block on the semaphore and are woken by `sem_post`.

**Goal:** POSIX-style counting semaphores (`sem_init`, `sem_wait`, `sem_post`).

- **Order of Learning:**
  1. **Semaphore Synchronization:**
     - **Concepts:** Atomic count decrements, task wait-queues for resource starvation prevention.
     - Files to inspect: `src/kernel/sync/sem.h`, `src/kernel/sync/sem.c`

---

### 24. Virtual Process Filesystem (/proc) (Phase 24)
**Status: real.** `/proc/version`, `uptime`, `meminfo`, `tasks` and `cpuinfo` are generated from live kernel state on every read.

**Goal:** Virtual `/proc` filesystem exposing system diagnostics (`/proc/meminfo`, `/proc/cpuinfo`, `/proc/uptime`).

- **Order of Learning:**
  1. **ProcFS Infrastructure:**
     - **Concepts:** Dynamic content generation on file read calls for system state.
     - Files to inspect: `src/fs/procfs.h`, `src/fs/procfs.c`

---

### 25. Teletype Subsystem (TTY) (Phase 25)
**Status: real.** Console reads go through a terminal layer in the kernel. Line mode (the default) echoes, handles backspace, Ctrl-U and Ctrl-D, and delivers whole lines; raw mode gives a program every key at once and is released automatically when that program ends. The PS/2 driver reports arrow keys, Home, End and Delete. One terminal only.

**Goal:** TTY line discipline, raw vs canonical mode, echo control.

- **Order of Learning:**
  1. **TTY Subsystem:**
     - **Concepts:** Input line editing, canonical input queue, signal generation (`Ctrl+C` -> `SIGINT`).
     - Files to inspect: `src/drivers/tty/tty.h`, `src/drivers/tty/tty.c`

---

### 26. Floating Point Unit (FPU/SSE) State Management (Phase 26)
**Status: real.** Each user task has its own x87/SSE state (FXSAVE area in `task_t`), saved and restored on every task switch. AVX is not enabled.

**Goal:** FPU/SSE register initialization (`CR0`/`CR4`), `fxsave`/`fxrstor` context switching.

- **Order of Learning:**
  1. **FPU / SSE Hardware Support:**
     - **Concepts:** Enabling SSE in `CR4`, saving 512-byte FPU/SSE register state on task switch.
     - Files to inspect: `src/arch/x86_64/fpu.h`, `src/arch/x86_64/fpu.c`

---

### 27. Paging Swap Subsystem (Phase 27)
**Status: stub.** A slot bitmap. No page is ever written out.

**Goal:** Out-of-memory page swapping to disk block storage (`swap_out`, `swap_in`).

- **Order of Learning:**
  1. **Swap Memory Engine:**
     - **Concepts:** Evicting anonymous physical pages to swap disk space, demand paging on swap fault.
     - Files to inspect: `src/mm/swap.h`, `src/mm/swap.c`

---

### 28. Dynamic Executable Linking (Phase 28)
**Status: stub.** Recognises relocation types and returns 0.

**Goal:** Parsing dynamic ELF section headers (`.dynamic`, `.got`, `.plt`), runtime symbol resolution.

- **Order of Learning:**
  1. **Dynamic ELF Resolver:**
     - **Concepts:** Global Offset Table (GOT) relocation, Procedure Linkage Table (PLT) stubs.
     - Files to inspect: `src/fs/elf_dyn.h`, `src/fs/elf_dyn.c`

---

### 29. UDP/IP Network Socket Stack (Phase 29)
**Status: real.** Ethernet, ARP, IPv4, ICMP echo and UDP, with sockets as file descriptors (`socket`, `bind`, `sendto`, `recvfrom`) and a `ping` call. Static configuration for QEMU's user-mode network (10.0.2.15, gateway 10.0.2.2). No TCP, no DHCP, no DNS, no IPv6, no fragmentation.

**Goal:** Network socket layer (`socket`, `bind`, `sendto`, `recvfrom`), IPv4/UDP packet handling.

- **Order of Learning:**
  1. **Network Sockets:**
     - **Concepts:** Socket descriptor table, UDP packet header building, IP checksum computation.
     - Files to inspect: `src/net/socket.h`, `src/net/socket.c`

---

### 30. Desktop Window Manager & GUI Compositor (Phase 30)
**Status: stub.** Allocates a pixel buffer. Nothing is composited.

**Goal:** Graphical GUI window creation (`window_create`), compositing back-buffer blitter.

- **Order of Learning:**
  1. **GUI Compositor:**
     - **Concepts:** Window z-ordering, title bar rendering, window drag/move handling.
     - Files to inspect: `src/gui/compositor/window.h`, `src/gui/compositor/window.c`

---

### 31. User Account Authentication (Phase 31)
**Status: stub.** Accepts `root` with any password.

**Goal:** User account authentication (`auth_login`), UID/GID context management.

- **Order of Learning:**
  1. **User Authentication:**
     - **Concepts:** User databases (`/etc/passwd`), password hash validation, process UID/GID checks.
     - Files to inspect: `src/kernel/proc/auth.h`, `src/kernel/proc/auth.c`

---

### 32. Kernel Ring Buffer & dmesg (Phase 32)
**Status: real.** Everything `kprintf` prints is kept in a 64 KiB ring buffer from the first boot message; `dmesg` (both shells) and `/proc/kmsg` read it.

**Goal:** Kernel syslog ring buffer (`klog_write`), `dmesg` shell command.

- **Order of Learning:**
  1. **Kernel Syslog Buffer:**
     - **Concepts:** Circular log buffer, log severity levels (INFO, WARN, ERROR), dmesg output.
     - Files to inspect: `src/kernel/log/klog.h`, `src/kernel/log/klog.c`

---

### 33. High-Resolution Timers & Nanosleep (Phase 33)
**Status: real.** The timestamp counter is calibrated against the timer at boot, giving a nanosecond clock (`clock_ns`) and sleeps accurate to microseconds (`nanosleep` sleeps whole ticks and watches the clock for the remainder). No one-shot hardware timer: wake-ups from sleep are still on tick boundaries.

**Goal:** Nanosecond precision sleep (`nanosleep`), HRT timer queue.

- **Order of Learning:**
  1. **High-Resolution Timers:**
     - **Concepts:** Nanosecond deadline queue, APIC timer hardware calibration.
     - Files to inspect: `src/kernel/time/hrtimer.h`, `src/kernel/time/hrtimer.c`

---

### 34. VFS Mount Manager (Phase 34)
**Status: real.** A record of which source provides which part of the tree (`/` memory, `/bin` initrd, `/proc`, `/dev`, `/disk` FAT32), shown by `mount` and `/proc/mounts`. Path lookup does not use it: every source attaches its nodes into the one tree. Nothing can be unmounted.

**Goal:** Path mount points (`vfs_mount`), mount table management, shell `mount` command.

- **Order of Learning:**
  1. **Mount Manager:**
     - **Concepts:** Binding filesystem instances to VFS directory paths (`/mnt`, `/dev`, `/proc`).
     - Files to inspect: `src/fs/mount.h`, `src/fs/mount.c`

---

### 35. POSIX Shared Memory (SHM) (Phase 35)
**Status: real.** Two kinds, both real: `mmap(MAP_SHARED)` memory stays shared between a process and the children it forks, and keyed segments (`shmget`, `shmat`, `shmrm`) are shared between any processes. Frames are reference counted and freed with the last mapping. No permissions.

**Goal:** Inter-process shared physical frame regions (`shm_create`, `shm_get`, `shm_destroy`).

- **Order of Learning:**
  1. **Shared Memory IPC:**
     - **Concepts:** Region creation (`shm_create`), region resolution (`shm_get`), region destruction (`shm_destroy`).
     - Files to inspect: `src/kernel/ipc/shm.h`, `src/kernel/ipc/shm.c`

---

### 36. Cryptographic Primitives (Phase 36)
**Status: real.** Many primitives, spot-checked against a few bytes of known answers. Not audited, and nothing in the kernel uses it.

**Goal:** Production-grade Kernel Cryptographic Suite (SHA-1, SHA-256, SHA-512, MD5, BLAKE2s, SipHash-2-4, AES-128/256, AES-XTS, AES-GCM, ChaCha20, Poly1305, HMAC, HKDF-SHA256, Ed25519, CRC32/CRC32C).

- **Order of Learning:**
  1. **Cryptographic Hashing & Checksums:**
     - **Concepts:** SHA-256 (`sha256_hash`), SHA-512 (`sha512_hash`), SHA-1 (`sha1_hash`), MD5 (`md5_hash`), BLAKE2s-256 (`blake2s_hash`), SipHash-2-4 (`siphash24`), CRC32/CRC32C (`crc32_calc`, `crc32c_calc`).
     - Files to inspect: `lib/crypto/sha256.c`, `lib/crypto/sha512.c`, `lib/crypto/sha1.c`, `lib/crypto/blake2s.c`, `lib/crypto/siphash.c`, `lib/crypto/crc32.c`
  2. **Symmetric Ciphers & Modes:**
     - **Concepts:** AES-128 & AES-256 (`aes128_encrypt_block`), IEEE 1619 AES-XTS (`aes128_xts_encrypt`), NIST SP 800-38D AES-GCM AEAD (`aes128_gcm_encrypt`), RFC 7539 ChaCha20 (`chacha20_encrypt`).
     - Files to inspect: `lib/crypto/aes.c`, `lib/crypto/aes_modes.c`, `lib/crypto/chacha20.c`
  3. **Key Derivation, MACs & Signatures:**
     - **Concepts:** RFC 7539 Poly1305 MAC (`poly1305_mac`), RFC 2104 Keyed-HMAC (`hmac_sha256`), RFC 5869 HKDF-SHA256 (`hkdf_sha256`), RFC 8032 Ed25519 Signatures (`ed25519_sign`, `ed25519_verify`).
     - Files to inspect: `lib/crypto/poly1305.c`, `lib/crypto/hmac.c`, `lib/crypto/hkdf.c`, `lib/crypto/ed25519.c`

---

### 37. Virtual Hardware Devices (/dev) (Phase 37)
**Status: real.** `/dev/null`, `/dev/zero`, `/dev/random` and `/dev/console` are device nodes with their own read/write functions. `/dev/random` is a seeded xorshift generator, not cryptographically secure.

**Goal:** Virtual character devices (`/dev/null`, `/dev/zero`, `/dev/random`), RDTSC pseudo-random number generator (`get_random_u32`).

- **Order of Learning:**
  1. **Virtual Device Tree:**
     - **Concepts:** Mount `/dev` nodes (`/dev/null`, `/dev/zero`, `/dev/random`), RDTSC PRNG (`get_random_u32`).
     - Files to inspect: `src/drivers/char/dev.h`, `src/drivers/char/dev.c`

---

### 38. Kernel Performance Benchmark Suite (Phase 38)
**Status: real.** `bench` times memcpy, memset, heap and frame allocation for 100 ms each and prints rates. Timer resolution limits accuracy to a few percent.

**Goal:** CPU MIPS calculation & memory throughput diagnostic suite.

- **Order of Learning:**
  1. **Benchmark Suite:**
     - **Concepts:** Memory copy benchmark, CPU loop benchmark (`bench_run_all`).
     - Files to inspect: `src/kernel/bench.h`, `src/kernel/bench.c`
