# BashketKernel

BashketKernel is a 64-bit x86_64 higher-half operating system kernel developed for learning kernel architecture, memory management, scheduling, storage, and modern hardware drivers.

## Status

The kernel boots under QEMU into a kernel-mode shell that can run user
programs in ring 3, including a user-space shell (`exec /bin/sh`) with
pipelines, redirection and job interruption. Processes with signals, users
and permissions, memory management, files, the terminal, the scheduler,
virtio disk and network drivers, a FAT32 filesystem and a small UDP/IP stack
are implemented and tested.

Everything in the table is implemented. Placeholder code that used to sit in
the tree (a mock bootloader, mouse, swap, window manager) has been deleted
rather than kept as stubs; phase numbers are the original ones, so there are
gaps. `PLAN.md` lists the known limits and proposes what to build next.

- **real**: implemented and exercised at runtime.
- **partial**: some of it works; the note says what does not.

| Phase | Subsystem | Status | Notes |
|---|---|---|---|
| 1 | Boot, console, serial | real | Framebuffer console, COM1 serial, `kprintf`. |
| 2 | GDT, TSS, IDT, interrupts | real | Double faults run on their own stack (IST1). Kernel code is read-only; NX, WP, and (where the CPU has them) SMEP, SMAP and UMIP are on. QEMU's default CPU has none of the last three: use `CPU=max`. |
| 3 | PMM, VMM, kernel heap | real | Bitmap frame allocator, 4-level paging, first-fit heap, per-process address spaces. |
| 4 | Multitasking | real | Preemptive round-robin kernel threads, guarded kernel stacks, zombie reaping. Tasks block (sleep, input, wait, semaphores) instead of polling. 100 Hz PIT tick. |
| 5 | Userspace and syscalls | real | Programs run in ring 3 with their own address space. `syscall`/`sysret` entry, validated user pointers, per-process file descriptors, and a hardened ELF64 loader. User programs are position-independent and loaded at a random address, with a randomised stack and mmap region (ASLR). |
| 6 | VFS and initrd | real | In-memory VFS tree (including `/tmp`), readable and writable (contents live in RAM and are lost at power-off), with per-process working directories and relative paths. A USTAR initrd with the user programs is loaded as a Limine module. |
| 7 | Shell | real | Two shells. The boot shell is a kernel thread with debugging commands. `/bin/sh` is a user program with pipelines and redirection, built only on system calls. |
| 8 | Slab, COW, mmap | partial | The slab allocator validates frees, honours alignment, returns empty pages, and backs open-file objects. Anonymous `mmap`, `munmap` and `brk` work per process. `fork` shares pages copy-on-write. The user stack grows on demand up to 8 MiB. `mmap` and `brk` memory is still allocated at once, and nothing is file-backed. |
| 9 | LAPIC and I/O APIC | real | ISA IRQs routed from the ACPI MADT. The LAPIC timer is not used. |
| 10 | SMP and spinlocks | partial | Every CPU the bootloader reports is started with its own per-CPU structure (reached through GS, with `swapgs` on every crossing between ring 3 and the kernel), task state segment and double-fault stack; it takes a breakpoint exception to prove it, reports in, and is parked. Only the boot CPU runs tasks so far, because many locks still assume one CPU. Spinlocks are real. |
| 11 | virtio-blk and FAT32 | real | virtio-blk is a real driver (legacy PCI interface, one virtqueue, interrupt-driven, one request at a time); `/dev/vda` is the raw disk. The FAT32 volume on it is mounted at `/disk`, readable and writable: files and directories can be created, written, appended, truncated and removed, programs can be run from it, and everything survives a reboot. Limits: 8.3 names only (no long names, no mixed case), no rename, no journal, and the directory tree is read once at boot. |
| 12 | fork, exec, wait, kill | real | `fork` (copy-on-write), `execve`, `waitpid`, `kill`, exit statuses. A faulting process is killed without affecting the kernel. Signals can end a process, be ignored, or run a handler in the program (`signal`), with the interrupted state saved on the user stack and restored by `sigreturn`. They take effect at safe points; a process asleep in a system call is woken, and for a handler the call returns `EINTR`. There is no signal masking and no automatic restart of an interrupted call. |
| 13 | PCI and virtio-net | real | PCI scan (function 0 of each device), configuration space access, shared interrupt lines. virtio-net is a real driver: receive and transmit virtqueues, interrupt-driven, MAC address from the device. |
| 16 | User programs | real | `user/*.c` are built into `/bin` with a small C library (`user/lib`): `sh`, `ls`, `cat`, `echo`, `wc`, `hello`, `fault`, `syscheck`, `fptest`, `spin`, `stress`, `fuzz`, `ps`, `kill`, `rm`, `mkdir`, `sleep`, `dmesg`, `env`, `diskio`, `clear`, `colors`, `ping`, `udpecho`, `udpsend`, `aslr`, `id`, `chmod`, `login`, and `edit`, a full-screen text editor. |
| 17 | RTC | real | Reads date and time from CMOS. |
| 18 | Environment variables | real | Each process has its own environment in its own memory, carried across `fork` and `execve`. The C library has `getenv`/`setenv`; `/bin/sh` has `export`, `$NAME`, `$?` and `PATH` lookup.  |
| 19 | Pipes | real | Blocking pipes behind file descriptors, with end-of-file and broken-pipe handling. Used by `/bin/sh` for `a \| b`. |
| 20 | ANSI colors | real | The console interprets ANSI escape sequences: 16 text and background colours, bold, cursor positioning and relative movement, erasing the screen or a line. `/bin/clear` and `/bin/colors` use them. Unsupported sequences are swallowed. |
| 22 | Priority scheduling | real | Round robin with weighted time slices: high, normal and low priority tasks run for 4, 2 and 1 ticks per turn, so priority sets the CPU share and nothing is starved. Per-task CPU ticks are in `/proc/tasks`. |
| 23 | Semaphores | real | Counting semaphores whose waiters block and are woken by `sem_post`. |
| 24 | ProcFS | real | `/proc/version`, `uptime`, `meminfo`, `tasks`, `cpuinfo`, `kmsg` and `mounts` are generated from live kernel state on every read. |
| 25 | TTY | real | Console reads go through a terminal layer in the kernel. Line mode (the default) echoes, handles backspace, Ctrl-U and Ctrl-D, and delivers whole lines; raw mode gives a program every key at once and is released automatically when that program ends. The PS/2 driver reports arrow keys, Home, End and Delete. One terminal only. |
| 26 | FPU/SSE | real | Each user task has its own x87/SSE state (FXSAVE), switched eagerly. AVX is not enabled. |
| 28 | ELF dynamic relocation | real | Static position-independent executables: the loader places them at a chosen or random address and applies their relative relocations, refusing any that would write outside the image. No shared libraries, no symbol resolution, no dynamic linker. |
| 29 | UDP/IP sockets | real | Ethernet, ARP, IPv4, ICMP echo and UDP, with sockets as file descriptors (`socket`, `bind`, `sendto`, `recvfrom`) and a `ping` call. Static configuration for QEMU's user-mode network (10.0.2.15, gateway 10.0.2.2). No TCP, no DHCP, no DNS, no IPv6, no fragmentation. |
| 31 | User authentication | real | Processes run as a numeric user (0 is root). Files and directories have an owner and read/write bits for the owner and for everyone else, checked on open, create, remove, `exec`, `chmod`/`chown`, `kill` and binding low ports. `/bin/login` checks a SHA-256 password hash from a root-only `/etc/passwd` and drops to the user. No groups, no execute bit, no set-uid programs (so no way back up to root), unsalted hashes. |
| 32 | Kernel log, `dmesg` | real | Everything `kprintf` prints is kept in a 64 KiB ring buffer from the first boot message; `dmesg` (both shells) and `/proc/kmsg` read it. |
| 33 | High-resolution timers | real | The timestamp counter is calibrated against the timer at boot, giving a nanosecond clock (`clock_ns`) and sleeps accurate to microseconds (`nanosleep` sleeps whole ticks and watches the clock for the remainder). No one-shot hardware timer: wake-ups from sleep are still on tick boundaries. |
| 34 | Mount table | real | A record of which source provides which part of the tree (`/` memory, `/bin` initrd, `/proc`, `/dev`, `/disk` FAT32), shown by `mount` and `/proc/mounts`. Path lookup does not use it: every source attaches its nodes into the one tree. Nothing can be unmounted. |
| 35 | Shared memory | real | Two kinds, both real: `mmap(MAP_SHARED)` memory stays shared between a process and the children it forks, and keyed segments (`shmget`, `shmat`, `shmrm`) are shared between any processes. Frames are reference counted and freed with the last mapping. No permissions. |
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
│   ├── console/         # Framebuffer console, bitmap font, ANSI escape sequence parser
│   ├── input/           # PS/2 keyboard
│   ├── net/             # virtio-net network card driver
│   ├── virtio/          # Virtqueues, shared by the virtio drivers
│   └── tty/             # Terminal line discipline
├── fs/                  # VFS, open files and descriptors, pipes, initrd, /proc, ELF loader, FAT32
├── include/             # Global test & subsystem headers
├── init/                # Kernel entry point (kmain in main.c)
├── kernel/
│   ├── ipc/             # Shared memory segments
│   ├── log/             # Kernel message log (dmesg, /proc/kmsg)
│   ├── proc/            # Processes (spawn, fork, exec, wait, kill), kernel shell, signals, users and permissions
│   ├── sched/           # Scheduler, task struct, blocking and wakeup, priorities
│   ├── sync/            # Spinlocks & Semaphores
│   ├── syscall/         # System call dispatcher & table
│   ├── time/            # PIT timer, sleeping
│   └── bench.{c,h}      # Timed benchmarks
├── lib/                 # Kernel utility functions (kprintf, kstring, string_util)
├── mm/                  # Frames, paging and address spaces, heap, slab, copy-on-write, mmap/brk
└── net/                 # Network stack: Ethernet, ARP, IPv4, ICMP, UDP

user/                    # Programs that run in ring 3, built into /bin of the initrd
├── lib/                 # Their C library, crt0 and system call wrappers
└── *.c                  # sh, ls, cat, echo, wc, ps, kill, ... and the test programs
disk/                    # Files put on the FAT32 disk image (build/disk.img), seen at /disk
tests/                   # In-kernel unit tests (make test)
tools/                   # qemu_shell.py (scripted boots), mkfat.py (disk image), symbolize.sh
```

---

## Unit Testing Framework

BashketKernel has a built-in unit test framework (`tests/`). `make test` builds a separate test kernel, boots it headless in QEMU, and exits non-zero if any test fails. The normal kernel (`make run`) does not include the tests.

`make test-lowmem` boots the normal kernel with 64 MB of RAM and runs `/bin/stress`, which exhausts memory in different ways (mappings, `brk`, a fork bomb, copy-on-write, a growing file) and checks that the kernel refuses cleanly and keeps working.

`make fuzz` runs `/bin/fuzz`, which makes random system calls with hostile arguments (kernel addresses, pointers at the edge of a mapping, huge lengths, closed descriptors) from ring 3. The kernel must survive every seed and pass `/bin/syscheck` afterwards. Seeds are fixed, so a failure can be reproduced with `make fuzz FUZZ_SEEDS=<n>`.

`make test-disk` checks persistence across two boots, first for a raw sector and then for files (created, appended, copied, removed and overwritten in the first boot, checked by `fsck.fat` on the host, read back in the second). It then boots against six deliberately damaged FAT32 images, reads and writes on them, and requires the kernel to carry on.

`make test-smp` runs the unit tests on a four-CPU machine and checks that all four CPUs started.

`make test-net` exchanges real UDP datagrams between the host and the kernel, in both directions, over the host's loopback interface.

`make test-login` scripts a login as an ordinary user and checks what that user can and cannot do.

`make test-edit` drives the full-screen editor with scripted keystrokes and checks the saved file.

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
BashketOS> exec /bin/login alice       # password "alice": a shell as an ordinary user
BashketOS> exec /bin/sh                # user-space shell (as root)
$ ls /bin | wc > /tmp/count
$ cat /tmp/count
$ ls -l /                              # permissions and owners
$ ping 10.0.2.2                        # QEMU's gateway answers
$ edit /disk/todo.txt                  # full-screen editor: arrows, Ctrl-S save, Ctrl-Q quit
$ spin                                 # loops forever; Ctrl-C stops it
$ exit
```

To add a program, put `name.c` in `user/`; it is built against `user/lib` and
appears as `/bin/name`.

### 5. Triggering kernel faults on purpose
The shell command `crash div0`, `crash pf` or `crash stack` raises a divide
error, a page fault or a kernel stack overflow, to check that each one is
caught and reported on the serial console.
