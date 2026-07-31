# BashketKernel

BashketKernel is a 64-bit x86_64 higher-half operating system kernel developed for learning kernel architecture, memory management, scheduling, storage, and modern hardware drivers.

## Status

The kernel boots under QEMU into a kernel-mode shell that can run user
programs in ring 3, including a user-space shell (`exec /bin/sh`) with
pipelines, redirection and job interruption. Processes, memory management,
the file layer, the terminal, the scheduler, a virtio disk driver and a
FAT32 filesystem are real and tested. Networking, the mouse, the window
manager and several later phases are still placeholders, and the table
below says which. `PLAN.md` tracks the work and ends with a list of
known gaps in the parts that are real.

- **real**: implemented and exercised at runtime.
- **partial**: some of it works; the note says what does not.
- **stub**: an interface with placeholder behaviour behind it.

| Phase | Subsystem | Status | Notes |
|---|---|---|---|
| 1 | Boot, console, serial | real | Framebuffer console, COM1 serial, `kprintf`. |
| 2 | GDT, TSS, IDT, interrupts | real | Double faults run on their own stack (IST1). Kernel code is read-only; NX, WP, and (where the CPU has them) SMEP, SMAP and UMIP are on. QEMU's default CPU has none of the last three: use `CPU=max`. |
| 3 | PMM, VMM, kernel heap | real | Bitmap frame allocator, 4-level paging, first-fit heap, per-process address spaces. |
| 4 | Multitasking | real | Preemptive round-robin kernel threads, guarded kernel stacks, zombie reaping. Tasks block (sleep, input, wait, semaphores) instead of polling. 100 Hz PIT tick. |
| 5 | Userspace and syscalls | real | Programs run in ring 3 with their own address space. `syscall`/`sysret` entry, validated user pointers, hardened ELF64 loader, per-process file descriptors. |
| 6 | VFS and initrd | real | In-memory VFS tree, readable and writable (contents live in RAM and are lost at power-off), with per-process working directories and relative paths. A USTAR initrd with the user programs is loaded as a Limine module. |
| 7 | Shell | real | Two shells. The boot shell is a kernel thread with debugging commands. `/bin/sh` is a user program with pipelines and redirection, built only on system calls. |
| 8 | Slab, COW, mmap | partial | The slab allocator validates frees, honours alignment, returns empty pages, and backs open-file objects. Anonymous `mmap`, `munmap` and `brk` work per process. `fork` shares pages copy-on-write. The user stack grows on demand up to 8 MiB. `mmap` and `brk` memory is still allocated at once, and nothing is file-backed. |
| 9 | LAPIC and I/O APIC | real | ISA IRQs routed from the ACPI MADT. The LAPIC timer is not used. |
| 10 | SMP and spinlocks | stub | Spinlocks are real. Only the boot CPU is ever started. |
| 11 | virtio-blk and FAT32 | real | virtio-blk is a real driver (legacy PCI interface, one virtqueue, interrupt-driven, one request at a time); `/dev/vda` is the raw disk. The FAT32 volume on it is mounted at `/disk`, readable and writable: files and directories can be created, written, appended, truncated and removed, programs can be run from it, and everything survives a reboot. Limits: 8.3 names only (no long names, no mixed case), no rename, no journal, and the directory tree is read once at boot. |
| 12 | fork, exec, wait, kill | real | `fork` (copy-on-write), `execve`, `waitpid`, `kill`, exit statuses. A faulting process is killed without affecting the kernel. Signals can end a process, be ignored, or run a handler in the program (`signal`), with the interrupted state saved on the user stack and restored by `sigreturn`. They take effect at safe points; a process asleep in a system call is woken, and for a handler the call returns `EINTR`. There is no signal masking and no automatic restart of an interrupted call. |
| 13 | PCI and virtio-net | partial | PCI scan works (function 0 only). virtio-net only logs. |
| 14 | Custom bootloader | stub | A CPUID check. Limine boots the kernel. |
| 15 | PS/2 mouse and cursor | stub | The IRQ handler discards mouse bytes. Nothing is drawn. |
| 16 | User programs | real | `user/*.c` are built into `/bin` with a small C library (`user/lib`): `sh`, `ls`, `cat`, `echo`, `wc`, `hello`, `fault`, `syscheck`, `fptest`, `spin`, `stress`, `fuzz`, `ps`, `kill`, `rm`, `mkdir`, `sleep`, `dmesg`, `env`, `diskio`, `clear`, `colors`. |
| 17 | RTC | real | Reads date and time from CMOS. |
| 18 | Environment variables | real | Each process has its own environment in its own memory, carried across `fork` and `execve`. The C library has `getenv`/`setenv`; `/bin/sh` has `export`, `$NAME`, `$?` and `PATH` lookup. (The kernel shell still has a separate global table for its `env`/`export` commands.) |
| 19 | Pipes | real | Blocking pipes behind file descriptors, with end-of-file and broken-pipe handling. Used by `/bin/sh` for `a \| b`. |
| 20 | ANSI colors | real | The console interprets ANSI escape sequences: 16 text and background colours, bold, cursor positioning and relative movement, erasing the screen or a line. `/bin/clear` and `/bin/colors` use them. Unsupported sequences are swallowed. |
| 21 | RamFS | stub | Creates `/tmp` in the VFS tree. Not a separate filesystem. |
| 22 | Priority scheduling | real | Round robin with weighted time slices: high, normal and low priority tasks run for 4, 2 and 1 ticks per turn, so priority sets the CPU share and nothing is starved. Per-task CPU ticks are in `/proc/tasks`. |
| 23 | Semaphores | real | Counting semaphores whose waiters block and are woken by `sem_post`. |
| 24 | ProcFS | real | `/proc/version`, `uptime`, `meminfo`, `tasks`, `cpuinfo` and `kmsg` are generated from live kernel state on every read. |
| 25 | TTY | real | Console reads go through a line discipline in the kernel: echo, backspace, Ctrl-U, Ctrl-D for end-of-file, and whole lines delivered on Enter. One terminal; no raw mode. |
| 26 | FPU/SSE | real | Each user task has its own x87/SSE state (FXSAVE), switched eagerly. AVX is not enabled. |
| 27 | Swap | stub | A slot bitmap. No page is ever written out. |
| 28 | ELF dynamic relocation | stub | Recognises relocation types and returns 0. |
| 29 | UDP/IP sockets | stub | Internet checksum works. No packets are built or sent. |
| 30 | Window manager | stub | Allocates a pixel buffer. Nothing is composited. |
| 31 | User authentication | stub | Accepts `root` with any password. |
| 32 | Kernel log, `dmesg` | real | Everything `kprintf` prints is kept in a 64 KiB ring buffer from the first boot message; `dmesg` (both shells) and `/proc/kmsg` read it. |
| 33 | High-resolution timers | partial | `nanosleep` blocks without using CPU, but its resolution is the 10 ms tick. |
| 34 | Mount table | partial | Records mount points; path lookup does not use them. |
| 35 | Shared memory | partial | Keyed kernel heap buffers. Not mapped into processes. |
| 36 | Crypto library | real | Many primitives, spot-checked against a few bytes of known answers. Not audited, and nothing in the kernel uses it. |
| 37 | /dev devices | real | `/dev/null`, `/dev/zero`, `/dev/random` and `/dev/console` are device nodes with their own read/write functions. `/dev/random` is a seeded xorshift generator, not cryptographically secure. |
| 38 | Benchmarks | real | `bench` times memcpy, memset, heap and frame allocation for 100 ms each and prints rates. Timer resolution limits accuracy to a few percent. |

---

## Directory Architecture

BashketKernel source code (`src/`) is organized into modular subsystem directories:

```
src/
├── arch/x86_64/         # CPU architecture, GDT/IDT/ISRs, APIC, SMP, FPU, low-level assembly
├── boot/                # Limine bootloader protocol handover & headers
├── drivers/
│   ├── block/           # virtio-blk disk driver
│   ├── bus/             # PCI Bus enumerator & config space
│   ├── char/            # Serial (COM1 UART), RTC, /dev virtual devices
│   ├── console/         # Framebuffer console & bitmap font blitter
│   ├── input/           # PS/2 keyboard; mouse and cursor (stubs)
│   ├── net/             # virtio-net (stub)
│   └── tty/             # Terminal line discipline
├── fs/                  # VFS, open files and descriptors, pipes, initrd, /proc, ELF loader, FAT32
├── gui/
│   └── compositor/      # ANSI escape sequence parser; window manager (stub)
├── include/             # Global test & subsystem headers
├── init/                # Kernel entry point (kmain in main.c)
├── kernel/
│   ├── ipc/             # Shared memory (partial)
│   ├── log/             # Kernel message log (dmesg, /proc/kmsg)
│   ├── proc/            # Processes (spawn, fork, exec, wait, kill), kernel shell, environment, auth (stub)
│   ├── sched/           # Scheduler, task struct, blocking and wakeup, priorities
│   ├── sync/            # Spinlocks & Semaphores
│   ├── syscall/         # System call dispatcher & table
│   ├── time/            # PIT timer, sleeping
│   └── bench.{c,h}      # Timed benchmarks
├── lib/                 # Kernel utility functions (kprintf, kstring, string_util)
├── mm/                  # Frames, paging and address spaces, heap, slab, copy-on-write, mmap/brk; swap (stub)
└── net/                 # Sockets (stub)

user/                    # Programs that run in ring 3, built into /bin of the initrd
├── lib/                 # Their C library, crt0 and system call wrappers
└── *.c                  # sh, ls, cat, echo, wc, ps, kill, ... and the test programs
disk/                    # Files put on the FAT32 disk image (build/disk.img), seen at /disk
tests/                   # In-kernel unit tests (make test)
tools/                   # qemu_shell.py (scripted boots), mkfat.py (disk image), symbolize.sh
```

---

## Unit Testing Framework

BashketKernel has a built-in unit test framework (`tests/`). `make test` builds a separate test kernel, boots it headless in QEMU, and exits non-zero if any test fails. The normal kernel (`make run`) does not include the tests. Tests for stub subsystems only check the placeholder behaviour.

`make test-lowmem` boots the normal kernel with 64 MB of RAM and runs `/bin/stress`, which exhausts memory in different ways (mappings, `brk`, a fork bomb, copy-on-write, a growing file) and checks that the kernel refuses cleanly and keeps working.

`make fuzz` runs `/bin/fuzz`, which makes random system calls with hostile arguments (kernel addresses, pointers at the edge of a mapping, huge lengths, closed descriptors) from ring 3. The kernel must survive every seed and pass `/bin/syscheck` afterwards. Seeds are fixed, so a failure can be reproduced with `make fuzz FUZZ_SEEDS=<n>`.

`make test-disk` checks persistence across two boots, first for a raw sector and then for files (created, appended, copied, removed and overwritten in the first boot, checked by `fsck.fat` on the host, read back in the second). It then boots against six deliberately damaged FAT32 images, reads and writes on them, and requires the kernel to carry on.

`make check` runs all of these on both CPU models.

All three targets accept `CPU=max` (for example `make test CPU=max`) to run on QEMU's fullest CPU model, where SMEP, SMAP and UMIP are active.

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
BashketOS> exec /bin/sh /etc/demo.sh   # a scripted tour of the system
BashketOS> cat disk/readme.txt         # a file on the FAT32 disk; under `make run`, files written to /disk survive a reboot
BashketOS> exec /disk/bin/hello        # a program loaded from the disk
BashketOS> exec /bin/sh                # user-space shell
$ ls /bin | wc > /tmp/count
$ cat /tmp/count
$ spin                                 # loops forever; Ctrl-C stops it
$ exit
```

To add a program, put `name.c` in `user/`; it is built against `user/lib` and
appears as `/bin/name`.

### 5. Triggering kernel faults on purpose
The shell command `crash div0`, `crash pf` or `crash stack` raises a divide
error, a page fault or a kernel stack overflow, to check that each one is
caught and reported on the serial console.
