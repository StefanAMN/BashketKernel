# BashketKernel

BashketKernel is a 64-bit x86_64 higher-half operating system kernel developed for learning kernel architecture, memory management, scheduling, storage, and modern hardware drivers.

## Feature Roadmap & Status (All 38 Phases Completed)

- **Phase 1: Boot & Console System** — Framebuffer console rendering, COM1 serial logging (`kprintf`).
- **Phase 2: CPU Architecture & Interrupts** — GDT, TSS, IDT, ISR assembly stubs, 8259 PIC remapping.
- **Phase 3: Memory Management** — Limine PMM frame allocator, 4-level paging VMM, higher-half mapping, first-fit kernel heap (`kmalloc`/`kfree`).
- **Phase 4: Multitasking & Scheduler** — Preemptive round-robin scheduler, assembly context switching (`ctx_switch.S`), 8254 PIT timer at 100 Hz.
- **Phase 5: Userspace & System Calls** — `syscall`/`sysret` MSR configuration, assembly entry trampoline, 18 POSIX system calls, ELF64 binary executable loader.
- **Phase 6: Virtual Filesystem (VFS)** — Hierarchical VFS tree nodes, file unlinking (`vfs_unlink`), metadata querying (`vfs_stat`), file writing (`vfs_write`), directory operations, USTAR tar ramdisk parser.
- **Phase 7: Interactive Kernel Shell** — PS/2 keyboard driver, scancode mapping, interactive shell with coreutils (`help`, `ls`, `cat`, `echo`, `mem`, `ps`, `uptime`, `date`, `env`, `export`, `unset`, `dmesg`, `mount`, `bench`, `touch`, `mkdir`, `rm`, `exec`, `kill`, `clear`).
- **Phase 8: Advanced Memory Management** — Slab cache object allocator (`kmem_cache_t`), Copy-on-Write (COW) frame refcounting, virtual `mmap`/`munmap` API.
- **Phase 9: Modern Interrupt Architecture** — Local APIC (LAPIC) driver and I/O APIC (IOAPIC) driver replacing 8259 PIC.
- **Phase 10: Symmetric Multiprocessing (SMP)** — Per-CPU context structures, multi-core BSP initialization, atomic spinlock primitives (`spinlock_t`).
- **Phase 11: Real Block Storage & Persistent Filesystem** — VirtIO Block Device driver (`virtio-blk`), FAT32 File System driver (`fat32`).
- **Phase 12: UNIX Process Lifecycle & Signals** — Process fork (`process_fork`), binary execution (`process_execve`), child waiting (`process_waitpid`), UNIX signals (`SIGKILL`, `SIGINT`).
- **Phase 13: PCI Bus & VirtIO Network Stack** — PCI Configuration Space bus scanner, VirtIO Network Card driver (`virtio-net`).
- **Phase 14: Custom x86_64 Bare-Metal Bootloader** — Stage 1 MBR bootsector, 64-bit Long Mode CPU capability verification (`CPUID`).
- **Phase 15: PS/2 Mouse Driver & Framebuffer Canvas** — PS/2 auxiliary mouse driver (IRQ 12 / Vector 44), 3-byte packet decoder, graphical cursor renderer.
- **Phase 16: Userland Environment & ELF Programs** — Built-in user binary suite (`/bin/hello`, `/bin/calc`), execution pipeline.
- **Phase 17: Real-Time Clock & CMOS Driver** — CMOS register access (`0x70`/`0x71`), wall-clock UTC time reading, shell `date` utility.
- **Phase 18: Process Environment Variables** — Process environment table, `env_get`, `env_set`, `env_unset`, `$VAR` shell expansion.
- **Phase 19: Inter-Process Communication & Pipes** — 4 KiB IPC ring-buffer stream pipes (`pipe_create`, `pipe_read`, `pipe_write`).
- **Phase 20: Terminal ANSI Color Decoder** — ANSI escape color decoder mapping ANSI 30..37 indices to RGB colors.
- **Phase 21: Dynamic RamFS & VFS Write Operations** — `/tmp` RAM filesystem, dynamic memory file growth, `vfs_write`.
- **Phase 22: Priority-Based Scheduler & Multi-Level Queues** — Execution priority levels (`PRIO_HIGH`, `PRIO_NORMAL`, `PRIO_LOW`), `task_set_priority`.
- **Phase 23: Counting Semaphores & Thread Sync** — Counting semaphores (`sem_t`, `sem_wait`, `sem_post`), atomic spinlock protection.
- **Phase 24: Pseudo-Filesystem ProcFS (`/proc`)** — Virtual `/proc` directory mounting `/proc/version`, `/proc/cpuinfo`.
- **Phase 25: Virtual Terminal TTY Device** — Virtual TTY input/output ring buffer stream device.
- **Phase 26: Hardware FPU & SSE State Management** — Control registers CR0/CR4 configuration, `fninit`, x87 FPU, MMX, and SSE hardware extensions enabled.
- **Phase 27: Virtual Memory Swap Manager** — Swap slot bitmap allocator, frame swap-out & swap-in manager.
- **Phase 28: ELF Dynamic Relocation Loader** — Dynamic symbol relocations (`R_X86_64_RELATIVE`, `R_X86_64_GLOB_DAT`, `R_X86_64_JUMP_SLOT`).
- **Phase 29: UDP/IP Network Socket Stack** — Socket layer (`socket_create`, `socket_sendto`), Internet checksumming.
- **Phase 30: Desktop Window Manager & GUI Compositor** — Graphical GUI window creation (`window_create`), canvas buffer allocation.
- **Phase 31: User Account Authentication** — User account authentication (`auth_login`), UID/GID context management.
- **Phase 32: Kernel Ring Buffer & dmesg** — Kernel syslog ring buffer (`klog_write`), `dmesg` shell command.
- **Phase 33: High-Resolution Timers & Nanosleep** — Nanosecond precision sleep (`nanosleep`), HRT timer queue.
- **Phase 34: VFS Mount Manager** — Path mount points (`vfs_mount`), mount table management, shell `mount` command.
- **Phase 35: POSIX Shared Memory (SHM)** — Inter-process shared physical frame regions (`shm_create`, `shm_get`, `shm_destroy`).
- **Phase 36: Cryptographic Primitives** — Production-grade Kernel Crypto Suite: SHA-1, SHA-256, SHA-512, MD5, BLAKE2s, SipHash-2-4, AES-128/256, AES-XTS, AES-GCM, ChaCha20, Poly1305, HMAC, HKDF, Ed25519, CRC32/CRC32C.
- **Phase 37: Virtual Hardware Devices (/dev)** — Virtual character devices (`/dev/null`, `/dev/zero`, `/dev/random`), RDTSC pseudo-random number generator (`get_random_u32`).
- **Phase 38: Kernel Performance Benchmark Suite** — Memory copy speed benchmark, CPU MIPS loop test, shell `bench` command (`bench_run_all`).

---

## Directory Architecture

BashketKernel source code (`src/`) is organized into modular subsystem directories:

```
src/
├── arch/x86_64/         # CPU architecture, GDT/IDT/ISRs, APIC, SMP, FPU, low-level assembly
├── boot/                # Limine bootloader protocol handover & headers
├── drivers/
│   ├── block/           # VirtIO Block storage driver
│   ├── bus/             # PCI Bus enumerator & config space
│   ├── char/            # Serial (COM1 UART), RTC, /dev virtual devices
│   ├── console/         # Framebuffer console & bitmap font blitter
│   ├── input/           # PS/2 Keyboard, Mouse & Cursor blitter
│   ├── net/             # VirtIO NIC driver
│   └── tty/             # TTY line discipline
├── fs/                  # VFS, InitRD, RAMFS, FAT32, ProcFS, Mount Manager, Pipes, ELF loaders
├── gui/
│   └── compositor/      # Desktop window manager/compositor & ANSI escape parser
├── include/             # Global test & subsystem headers
├── init/                # Kernel entry point (kmain in main.c)
├── kernel/
│   ├── ipc/             # POSIX Shared Memory (SHM)
│   ├── log/             # Kernel syslog ring buffer (klog & dmesg)
│   ├── proc/            # Process control, User apps, Environment vars, Auth, Shell
│   ├── sched/           # Task scheduler, Task Control Block, Priority scheduling
│   ├── sync/            # Spinlocks & Semaphores
│   ├── syscall/         # System call dispatcher & table
│   ├── time/            # High-resolution timers (hrtimer & nanosleep), PIT driver
│   └── bench.{c,h}      # Kernel diagnostic benchmark suite
├── lib/                 # Kernel utility functions (kprintf, kstring, string_util)
├── mm/                  # Memory Management (PMM, VMM, Heap, Slab, COW, MMAP, Swap)
└── net/                 # UDP/IP Network socket stack
```

---

## Unit Testing Framework

BashketKernel features a built-in unit test framework with **342 automated kernel unit tests** covering all 38 phases. All tests execute automatically during kernel boot.

---

## Prerequisites

To compile and run BashketKernel, you need:
- A C compiler suite (`gcc`, `make`, `ld`)
- `xorriso` (for building the bootable ISO image)
- `qemu-system-x86_64` (for hardware emulation)
- `gdb` (optional, for debugging)

### Automated Setup

We provide a setup script that automatically detects your package manager and installs dependencies:

```bash
chmod +x setup.sh
./setup.sh
```

---

## Building and Running

The project uses `Makefile` to handle building the kernel, creating the ISO image, and booting in QEMU.

### 1. Build the ISO
To compile the kernel and package it into `build/BashketKernel.iso`:
```bash
make iso
```

### 2. Run in QEMU
To boot the generated ISO in QEMU:
```bash
make run
```

### 3. Debugging with GDB
To start QEMU in suspended mode waiting for GDB:
```bash
make run-gdb
```
*(In a separate terminal, run `gdb build/kernel.elf` and type `target remote :1234`)*
