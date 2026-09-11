# BashketKernel

BashketKernel is a 64-bit x86_64 higher-half operating system kernel developed for learning kernel architecture, memory management, scheduling, storage, and modern hardware drivers.

## Status

The kernel boots under QEMU into a kernel-mode shell that can run user
programs in ring 3. The core (phases 1 to 6, 12 and 16) is real; much of the
rest is placeholder code. `PLAN.md` is the plan for closing
that gap.

- **real**: implemented and exercised at runtime.
- **partial**: some of it works; the note says what does not.
- **stub**: an interface with placeholder behaviour behind it.

| Phase | Subsystem | Status | Notes |
|---|---|---|---|
| 1 | Boot, console, serial | real | Framebuffer console, COM1 serial, `kprintf`. |
| 2 | GDT, TSS, IDT, interrupts | real | Double faults run on their own stack (IST1). |
| 3 | PMM, VMM, kernel heap | real | Bitmap frame allocator, 4-level paging, first-fit heap, per-process address spaces. |
| 4 | Multitasking | real | Preemptive round-robin kernel threads, guarded kernel stacks, zombie reaping. Tasks block (sleep, input, wait, semaphores) instead of polling. 100 Hz PIT tick. |
| 5 | Userspace and syscalls | real | Programs run in ring 3 with their own address space. `syscall`/`sysret` entry, validated user pointers, hardened ELF64 loader, per-process file descriptors. |
| 6 | VFS and initrd | real | In-memory VFS tree, readable and writable (contents live in RAM and are lost at power-off). A USTAR initrd with the user programs is loaded as a Limine module. |
| 7 | Shell | real | Two shells. The boot shell is a kernel thread with debugging commands. `/bin/sh` is a user program with pipelines and redirection, built only on system calls. |
| 8 | Slab, COW, mmap | partial | Slab allocator works. Anonymous `mmap`, `munmap` and `brk` work per process. `fork` shares pages copy-on-write. Nothing is demand-paged or file-backed. |
| 9 | LAPIC and I/O APIC | real | ISA IRQs routed from the ACPI MADT. The LAPIC timer is not used. |
| 10 | SMP and spinlocks | stub | Spinlocks are real. Only the boot CPU is ever started. |
| 11 | virtio-blk and FAT32 | stub | A RAM buffer and an in-memory file table. No virtqueues, no on-disk format. |
| 12 | fork, exec, wait, kill | real | `fork` (copy-on-write), `execve`, `waitpid`, `kill`, exit statuses. A faulting process is killed without affecting the kernel. No signal handlers. |
| 13 | PCI and virtio-net | partial | PCI scan works (function 0 only). virtio-net only logs. |
| 14 | Custom bootloader | stub | A CPUID check. Limine boots the kernel. |
| 15 | PS/2 mouse and cursor | stub | The IRQ handler discards mouse bytes. Nothing is drawn. |
| 16 | User programs | real | `user/*.c` are built into `/bin` with a small C library (`user/lib`): `sh`, `ls`, `cat`, `echo`, `wc`, `hello`, `fault`, `syscheck`, `fptest`. |
| 17 | RTC | real | Reads date and time from CMOS. |
| 18 | Environment variables | real | One global table, not per process. |
| 19 | Pipes | real | Blocking pipes behind file descriptors, with end-of-file and broken-pipe handling. Used by `/bin/sh` for `a \| b`. |
| 20 | ANSI colors | stub | A lookup from color code to RGB. No escape-sequence parser. |
| 21 | RamFS | stub | Creates `/tmp` in the VFS tree. Not a separate filesystem. |
| 22 | Priority scheduling | stub | Priorities are stored; the scheduler ignores them. |
| 23 | Semaphores | real | Counting semaphores whose waiters block and are woken by `sem_post`. |
| 24 | ProcFS | stub | Two static text files. |
| 25 | TTY | stub | A ring buffer. No line discipline. |
| 26 | FPU/SSE | real | Each user task has its own x87/SSE state (FXSAVE), switched eagerly. AVX is not enabled. |
| 27 | Swap | stub | A slot bitmap. No page is ever written out. |
| 28 | ELF dynamic relocation | stub | Recognises relocation types and returns 0. |
| 29 | UDP/IP sockets | stub | Internet checksum works. No packets are built or sent. |
| 30 | Window manager | stub | Allocates a pixel buffer. Nothing is composited. |
| 31 | User authentication | stub | Accepts `root` with any password. |
| 32 | Kernel log, `dmesg` | real | 4 KiB ring buffer. |
| 33 | High-resolution timers | partial | `nanosleep` blocks without using CPU, but its resolution is the 10 ms tick. |
| 34 | Mount table | partial | Records mount points; path lookup does not use them. |
| 35 | Shared memory | partial | Keyed kernel heap buffers. Not mapped into processes. |
| 36 | Crypto library | real | Many primitives, spot-checked against a few bytes of known answers. Not audited, and nothing in the kernel uses it. |
| 37 | /dev devices | stub | Static files. `/dev/random` contains the text `RND`. |
| 38 | Benchmarks | real | Memory copy and CPU loop timing. |

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

BashketKernel has a built-in unit test framework (`tests/`). `make test` builds a separate test kernel, boots it headless in QEMU, and exits non-zero if any test fails. The normal kernel (`make run`) does not include the tests. Tests for stub subsystems only check the placeholder behaviour.

---

## Prerequisites

To compile and run BashketKernel, you need:
- A C compiler suite (`gcc`, `make`, `ld`)
- `xorriso` (for building the bootable ISO image)
- `git` (the first build downloads Limine v8.7.0 into `limine/`)
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

### 4. Running user programs
`exec <path> [args]` in the shell runs a program from the initrd in ring 3 and
waits for it:

```
BashketOS> exec /bin/hello one two
BashketOS> exec /bin/syscheck          # exercises the system call interface
BashketOS> exec /bin/fault kread       # tries to read kernel memory; gets killed
BashketOS> exec /bin/sh                # user-space shell
$ ls /bin | wc > /tmp/count
$ cat /tmp/count
$ exit
```

To add a program, put `name.c` in `user/`; it is built against `user/lib` and
appears as `/bin/name`.

### 5. Triggering kernel faults on purpose
The shell command `crash div0`, `crash pf` or `crash stack` raises a divide
error, a page fault or a kernel stack overflow, to check that each one is
caught and reported on the serial console.
