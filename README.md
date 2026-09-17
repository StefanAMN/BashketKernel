# BashketKernel

BashketKernel is a 64-bit x86_64 higher-half operating system kernel developed for learning kernel architecture, memory management, scheduling, storage, and modern hardware drivers.

## Status

The kernel boots under QEMU into a kernel-mode shell that can run user
programs in ring 3, including a user-space shell (`exec /bin/sh`) with
pipelines, redirection and job interruption. Processes with signals, users
and permissions, memory management, files, the terminal, the scheduler,
virtio disk and network drivers, a FAT32 filesystem and a small TCP/IP stack
are implemented and tested.

Everything in the table is implemented. Placeholder code that used to sit in
the tree (a mock bootloader, mouse, swap, window manager) has been deleted
rather than kept as stubs; phase numbers are the original ones, so there are
gaps. `PLAN.md` lists the known limits and proposes what to build next.

- **real**: implemented and exercised at runtime.
- **partial**: some of it works; the note says what does not.

| Phase | Subsystem | Status | Notes |
|---|---|---|---|
| 1 | Boot, console, serial | real | Limine with two boot entries: the system, which starts `/bin/init` (it runs `/etc/rc`, then keeps a `login` prompt on the console and starts a new one after every session, `make test-boot`), and a developer entry whose command line (`shell`) gives the console to the kernel's own shell instead (`make run-dev`, and every scripted test). `shutdown` ends every process, marks the system disk clean and switches the machine off through ACPI (the PM1 control register from the FADT, the sleep type from the `\_S5` object in the DSDT); `shutdown -r` restarts it (the FADT's reset register, or the keyboard controller). `make test-shutdown`. Framebuffer console, COM1 serial, `kprintf`. |
| 2 | GDT, TSS, IDT, interrupts | real | Double faults run on their own stack (IST1). Kernel code is read-only; NX, WP, and (where the CPU has them) SMEP, SMAP and UMIP are on. QEMU's default CPU has none of the last three: use `CPU=max`. |
| 3 | PMM, VMM, kernel heap | real | Bitmap frame allocator, 4-level paging, a heap with free lists by size class (freed memory is poisoned and checked when handed out again, so a write after free is caught), per-process address spaces. |
| 4 | Multitasking | real | Preemptive round-robin kernel threads, guarded kernel stacks, zombie reaping. Tasks block (sleep, input, wait, semaphores) instead of polling. A running task is interrupted every 10 ms (see row 33 for what does the interrupting). |
| 5 | Userspace and syscalls | real | Programs run in ring 3 with their own address space. `syscall`/`sysret` entry, validated user pointers, per-process file descriptors, and a hardened ELF64 loader. User programs are position-independent and loaded at a random address, with a randomised stack and mmap region (ASLR). |
| 6 | VFS and initrd | real | One tree, with nodes from the system disk (`/`), the FAT volume, and memory (`/tmp`, `/dev`, `/proc`), per-process working directories and relative paths. `truncate` and `ftruncate` set a file's length either way and `utime` its time, on memory, FAT and ext2 (`/bin/truncate`, `/bin/touch`). `stat`, `lstat` and `fstat` describe a file (type, owner, mode, size, link count, inode number, time; `/bin/stat`, `ls -l`) and an open pipe, socket or terminal. A USTAR initrd with the programs and `/etc` is loaded as a Limine module: without a system disk it fills a root kept in memory, the rescue system (`make test-rescue`), whose changes are lost at power-off. |
| 7 | Shell | real | Two shells. The boot shell is a kernel thread with debugging commands. `/bin/sh` is a user program with pipelines, redirection of any descriptor (`2> file`, `2>&1`, applied left to right) and command lists (`;`, `&&`, `||`), `if`, `while`, `until`, `for` and `case` (nested, over several lines, with `break`, `continue` and a built-in `test` / `[`), functions (`f() { ...; }` with `$1`, `$#`, `"$@"` and `return`), wildcards (`*`, `?`, `[...]`), double and single quotes (parts of a word join, as in `"a b"c`), command substitution (`$(...)`), variables of the shell and of the environment (`X=1`, `${X}`, `export`, `unset`, `X=1 program` for one program), the usual text tools (`cp`, `head`, `tail`, `grep`, `sort`, `uniq`, `cut`, `tr`, `tee`, `xargs`, `seq`, `basename`, `dirname`, `find`, `du`, reading lines of any length with `user/lib/lines.c`), and at the terminal line editing with a history and Tab completion (the arrow keys, Home, End, Ctrl-A/E/K/U/W; `user/lib/lineedit.c`), built only on system calls. |
| 8 | Slab, COW, lazy and file-backed mmap | partial | The slab allocator validates frees, honours alignment, returns empty pages, and backs open-file objects; freed objects are poisoned and checked when handed out again, so a write after free panics, naming the cache (`slab_catches_writes_after_free`). `fork` shares pages copy-on-write. `mmap` and `brk` only promise memory: a page gets its frame when it is first touched (so does the user stack, up to 8 MiB), and the calls fail when more would be promised than is free. A file can be mapped privately (`mmap` with a descriptor): each page is read from the file when first touched, and writes stay in the process. `munmap` can remove part of a mapping. Not done: shared file mappings and writing changes back, a page cache (two processes mapping one file each read their own copy), and loading executables this way. |
| 9 | LAPIC and I/O APIC | real | ISA IRQs routed from the ACPI MADT. The local APIC timer is each CPU's one-shot timer (row 33). |
| 10 | SMP and spinlocks | partial | Every CPU the bootloader reports is started with its own per-CPU structure (reached through GS, with `swapgs` on every crossing between ring 3 and the kernel), task state segment and double-fault stack, and runs tasks: each has an idle task and its own local APIC timer, an idle CPU is woken by an inter-processor interrupt when a task becomes runnable, tasks move between CPUs (or are tied to one), and removing a kernel mapping waits for every CPU to drop its cached copy (TLB shootdown). Shared structures are protected by ranked spinlocks and mutexes, with the lock order checked at run time. Not done: one scheduler lock and task list for all CPUs (no per-CPU queues), and all device interrupts go to the boot CPU. |
| 11 | virtio-blk and FAT32 | real | virtio-blk is a real driver (one virtqueue, interrupt-driven, up to eight requests with the device at once); `/dev/vda` is the raw disk. A write-through cache of 1024 sectors sits between the filesystem and the driver. The FAT32 volume on it is mounted at `/disk`, readable and writable: files and directories can be created, written, appended, truncated, renamed, moved and removed, programs can be run from it, and everything survives a reboot. Long file names are read and written (ASCII; up to 127 characters), with invented short aliases; names keep their case and are unique without regard to it. A directory is read from the disk when something first looks inside it. Entries carry the date and time from the RTC. `fsck.fat` accepts what the kernel writes. Limits: no journal, writes are not delayed or batched, names outside ASCII are shown by their short alias, looking a name up is case-sensitive, and FAT stores no owners or permissions. |
| 12 | Processes, signals and jobs | partial | `fork` (copy-on-write; it refuses rather than leave no memory for the copies), `execve`, `waitpid` (with `WNOHANG` and `WUNTRACED`), `kill` (a negative number signals a process group), exit statuses, `dup`, `pipe2` and close-on-exec. A faulting process is killed without affecting the kernel. Signals can end a process, stop and continue it (`SIGSTOP`, `SIGTSTP`, `SIGCONT`), be ignored, or run a handler in the program (`signal`, `sigaction`), with the interrupted state saved on the user stack and restored by `sigreturn`. They can be blocked (`sigprocmask`; a handler's own signal is held back while it runs), a handler set with `SA_RESTART` restarts the `read`, `write`, `waitpid` or `accept` it interrupted, and `SIGCHLD` tells a parent that a child has exited or stopped. Signals take effect at safe points. `/bin/init` adopts processes whose parent has died and waits for them. Process groups and a terminal foreground group give the user shell job control (`&`, Ctrl-Z, `jobs`, `fg`, `bg`). Not done: there are no sessions, and a background job that reads the terminal is not stopped. |
| 13 | PCI and virtio-net | real | PCI scan (function 0 of each device), configuration space access, capability list, shared interrupt lines. The virtio drivers reach their devices through either of virtio's register interfaces: the modern one (registers in memory, found through the capability list, 64-bit feature negotiation) when the device offers it, the legacy I/O port block otherwise; `make check` runs the tests with devices that offer both, only the legacy and only the modern one. virtio-net is a real driver: receive and transmit virtqueues, interrupt-driven, MAC address from the device. With the modern interface the devices interrupt by MSI-X: each disk, and the card's receive queue, has a vector of its own, sent straight to one CPU's local APIC (the devices are spread over the CPUs, the boot CPU last); with the legacy one they share their PCI line through the I/O APIC. The boot log names which, every test run refuses the wrong one, and `/proc/interrupts` counts each line and vector per CPU. |
| 16 | User programs | real | `user/*.c` are built into `/bin` with a small C library (`user/lib`): `sh`, `ls`, `cat`, `echo`, `wc`, `hello`, `fault`, `syscheck`, `fptest`, `spin`, `stress`, `fuzz`, `ps`, `kill`, `rm`, `mkdir`, `sleep`, `dmesg`, `env`, `diskio`, `clear`, `colors`, `ping`, `udpecho`, `udpsend`, `nc`, `tcpecho`, `dhcp`, `host`, `init`, `orphan`, `mapread`, `su`, `passwd`, `chown`, `mv`, `fsstress`, `paint`, `swapper`, `shutdown`, `aslr`, `id`, `chmod`, `login`, and `edit`, a full-screen text editor. |
| 17 | RTC and time of day | real | Reads the CMOS clock once at boot and keeps the time of day from the nanosecond clock; `date` shows it and root sets it (`date -s`); file times on both filesystems come from it, kept as Unix seconds. UTC only. |
| 18 | Environment variables | real | Each process has its own environment in its own memory, carried across `fork` and `execve`. The C library has `getenv`/`setenv`; `/bin/sh` has `export`, `$NAME`, `$?` and `PATH` lookup.  |
| 19 | Pipes and poll | real | Blocking pipes behind file descriptors, with end-of-file and broken-pipe handling. Used by `/bin/sh` for `a \| b`. `poll` waits on pipes, the terminal, sockets and the mouse at once (`/bin/nc` uses it). |
| 20 | ANSI colors | real | The console interprets ANSI escape sequences: 16 text and background colours, bold, cursor positioning and relative movement, erasing the screen or a line. `/bin/clear` and `/bin/colors` use them. Unsupported sequences are swallowed. |
| 22 | Priority scheduling | real | Round robin with weighted time slices: high, normal and low priority tasks run for 4, 2 and 1 ticks per turn, so priority sets the CPU share and nothing is starved. Per-task CPU ticks are in `/proc/tasks`. |
| 23 | Semaphores | real | Counting semaphores whose waiters block and are woken by `sem_post`. |
| 24 | ProcFS | real | `/proc/version`, `uptime`, `meminfo`, `tasks`, `cpuinfo`, `kmsg`, `mounts` and `interrupts` are generated from live kernel state on every read. `/proc/<pid>/` appears with each task and goes once it is reaped (kept in step on every lookup), with `status` (name, state, ids, memory) and `cmdline` (its arguments); `/proc/self` is the reader's own. `ps` reads them. |
| 25 | TTY | real | Console reads go through a terminal layer in the kernel. Line mode (the default) echoes, handles backspace, Ctrl-U and Ctrl-D, and delivers whole lines; raw mode gives a program every key at once and is released automatically when that program ends. The PS/2 driver reports arrow keys, Home, End and Delete. One terminal only. |
| 26 | FPU/SSE | real | Each user task has its own x87/SSE state (FXSAVE), switched eagerly. AVX is not enabled. |
| 28 | ELF dynamic relocation | real | Static position-independent executables: the loader places them at a chosen or random address and applies their relative relocations, refusing any that would write outside the image. No shared libraries, no symbol resolution, no dynamic linker. |
| 29 | UDP and TCP sockets over IPv4, DHCP, DNS | partial | Ethernet, ARP (answers older than a minute are asked for again), IPv4, ICMP echo, UDP (64 KiB queued per socket) and TCP, with sockets as file descriptors (`socket`, `bind`, `sendto`, `recvfrom`; `connect`, `listen`, `accept`, `shutdown`, and `read`/`write` on a connection) and a `ping` call. TCP has the handshake, retransmission with back-off, flow control by the receiver's window, orderly close and reset; it has no congestion control, does not keep out-of-order segments, and uses a fixed initial retransmission time (see `src/net/tcp.h`). The interface starts with the addresses QEMU's user-mode network expects (10.0.2.15, gateway 10.0.2.2); `/bin/dhcp` replaces them with what a DHCP server offers (`dhcp -d` stays and renews the lease at T1 from the server that gave it, rebinds by broadcast at T2 and starts over when it runs out, RFC 2131 4.4.5), and names are resolved by a DNS client in the C library (`host`, `ping NAME`; A records only, no cache). No IPv6, no fragmentation. |
| 31 | Users, groups and permissions | partial | Processes run as a user and a group (numbers; root is 0). Files have an owner, a group and Unix permission bits (read, write and execute for owner, group and others), checked on open, create, remove, `execve` and `chmod`, and every directory a path passes through needs search (execute) permission; new files get 0666 less the process's `umask`. A set-uid file runs with its owner's rights while the process keeps its real user id (`getuid` / `geteuid`); the bit is dropped when a non-root user writes to the file or the file changes owner. `/bin/login`, `/bin/su` and `/bin/passwd` (the last two set-uid root) check passwords stored in `/etc/passwd` as salted PBKDF2-HMAC-SHA256 hashes. A process may also be in up to 16 supplementary groups (`setgroups`, root only, and `getgroups`), set at login from `/etc/group`, kept across `fork` and `execve`, and counted by every permission check; `id` lists them, and an owner may give a file to any group they are in. A set-gid file (02000) runs with the file's group as the effective group (`getegid`; `getgid` stays the caller's), and both bits are taken away when anyone but root writes to the file or gives it away. `/tmp` is sticky (01777): everyone may create there, but only an entry's owner (or the directory's, or root) may remove or rename it; elsewhere, the right to write in the directory is enough. `ls -l` shows `s` and `t`. Shared memory segments belong to their creator. Not done: set-gid on a directory (new files there do not take its group); files on the FAT volume all belong to root (FAT cannot store owners). (`/etc/passwd` is on the ext2 system disk, so a changed password survives a reboot, which `make test-shutdown` checks; only the rescue system, with no system disk, loses it.) |
| 32 | Kernel log, `dmesg` | real | Everything `kprintf` prints is kept in a 64 KiB ring buffer from the first boot message; `dmesg` (both shells) and `/proc/kmsg` read it. |
| 33 | High-resolution and one-shot timers | real | The timestamp counter is calibrated against the PIT at boot, giving a nanosecond clock (`clock_ns`). The kernel then stops the periodic tick: the tick count is read off the clock, and each CPU's local APIC timer is set to fire once, when that CPU next has something to do (10 ms on while it runs a task, a sleeper's deadline, output to show). An idle CPU is not interrupted at all (four idle CPUs: 7 timer interrupts in three seconds instead of 1200; `/proc/timers` counts them), and `sleep` and `nanosleep` end when asked, not at the next tick. On a CPU without a timestamp counter the kernel keeps the 100 Hz tick; that fallback is run by `make check` (`-cpu qemu64,-tsc`). |
| 34 | Mount table | real | Mount points: `/dev`, `/proc`, `/tmp` and the FAT32 volume at `/disk` sit on a directory the kernel puts in place (`vfs_mount_dir`). On a directory read from a disk, the disk's own entry of that name is hidden, not changed, and comes back when the mount point is taken away; a mount point cannot be removed or renamed (`EBUSY`). A record of which source provides which part of the tree is shown by `mount` and `/proc/mounts`. Path lookup does not use it: every source attaches its nodes into the one tree. Only an empty memory directory can be unmounted; volumes stay mounted. |
| 35 | Shared memory | real | Two kinds, both real: `mmap(MAP_SHARED)` memory stays shared between a process and the children it forks, and keyed segments (`shmget`, `shmat`, `shmrm`) are shared between any processes. Frames are reference counted and freed with the last mapping. No permissions. |
| 36 | Crypto library | real | SHA-256 (password hashing, generator rekeying) and ChaCha20 (the random generator), each checked against published test vectors. Nothing else: the unused algorithms were deleted. |
| 37 | /dev devices | real | `/dev/null`, `/dev/zero`, `/dev/random` and `/dev/console` are device nodes with their own read/write functions. `/dev/random` gives output of the kernel's generator: ChaCha20 with the key replaced after every request, seeded from RDSEED or RDRAND where the CPU has them and otherwise only from interrupt timing, which is weak right after boot (the boot log says which). |
| 38 | Benchmarks | real | `bench` times memcpy, memset, heap and frame allocation for 100 ms each and prints rates. Timer resolution limits accuracy to a few percent. |
| 39 | ext2 filesystem | partial | The system disk: an ext2 volume on the second virtio disk is the root of the file tree, with the programs in `/bin`, `/etc` and the users' home directories; files have owners, groups, permission bits and times that are stored on the disk and survive a reboot (a changed password included). Reading and writing (through the doubly indirect block), create, remove, `mkdir`, rename and move, truncation; `chmod` and `chown` are recorded; symbolic links (short ones inside the inode, longer ones in a block) and hard links, made with `ln` and `ln -s`, accepted by `e2fsck`, and links made on the host by `mke2fs` are followed. Built on the host with `mke2fs` and `debugfs` (`tools/mkroot.sh`); `e2fsck` finds nothing to fix after the kernel has written to it, and images damaged on purpose do not bring the kernel down. The volume carries a clean mark (the superblock's state) only while it is not in use: it is taken away at mount and put back by `shutdown`. When it is missing at mount, the last session ended in a crash, and the whole volume is checked: what an interrupted write can leave behind (blocks and inodes nothing refers to, link counts, sizes and free counts that are out of date) is put right: `make test-crash` kills QEMU in the middle of a write workload again and again and has `e2fsck` confirm the result. Not done: links, device files, the triply indirect block, a journal (data being written when the power goes is lost or partial; the structure is not). |
| 40 | Mouse and framebuffer devices | partial | A PS/2 mouse driver (the keyboard controller's second port, IRQ 12; three-byte packets decoded with resynchronisation) read by programs from `/dev/mouse`, and the screen as a file of pixels, `/dev/fb0` (`fb_info()` gives its geometry). A program that opens the screen for writing has it to itself: the console keeps its text up to date in RAM and on serial and repaints when the program closes the device or dies. `/bin/paint` draws with the mouse; `make test-paint` moves and clicks the mouse through QEMU's monitor and checks screenshots. Not done: mapping the screen into a program (`mmap`), a wheel, waiting for the mouse and the keyboard at once (there is no `poll`), and any window system. |
| 41 | Swap | partial | With a swap disk attached (one made by the host's `mkswap`; the kernel checks its header), a process that page-faults while memory is short pushes its own least recently used pages out to the disk (the clock algorithm, on the accessed bit) and they are read back when touched; `mmap` and `brk` may then promise more memory than the machine has. A swapped page is a not-present page-table entry holding its slot; `fork` shares the slot and each side reads its own copy. `/bin/swapper 80` runs on a 64 MiB machine (`make test-swap`); `/proc/meminfo` shows swap totals and counts. A system call whose buffer needs frames when none are free makes room the same way, from the caller's pages outside every buffer the call has checked (they are recorded as it checks them: `process_populate`), so a 4 MiB `read` into swapped-out memory with 1 MiB free succeeds. Limits, by design: a process only gives up its own pages (never another process's), at a fault from user mode, at `fork` or for a system call's buffers; frames still shared after a `fork` stay; a page read back is written again in full when it next goes out; one swap device; swap is attached only where asked (`tools/qemu_shell.py --swap-disk`). |

---

## Directory Architecture

BashketKernel source code (`src/`) is organized into modular subsystem directories:

```
src/
├── arch/x86_64/         # CPU architecture, GDT/IDT/ISRs, APIC, SMP, FPU, low-level assembly
├── boot/                # Limine bootloader protocol handover & headers
├── drivers/
│   ├── block/           # virtio-blk disk driver (several disks), sector cache
│   ├── bus/             # PCI Bus enumerator & config space
│   ├── char/            # Serial (COM1 UART), RTC, /dev virtual devices
│   ├── console/         # Framebuffer console, bitmap font, ANSI escape sequence parser
│   ├── input/           # PS/2 keyboard
│   ├── net/             # virtio-net network card driver
│   ├── virtio/          # What the virtio drivers share: the PCI register interfaces (legacy and modern), virtqueues
│   └── tty/             # Terminal line discipline
├── fs/                  # VFS, open files and descriptors, pipes, initrd, /proc, ELF loader, FAT32, ext2
├── include/             # Global test & subsystem headers
├── init/                # Kernel entry point (kmain in main.c)
├── kernel/
│   ├── ipc/             # Shared memory segments
│   ├── log/             # Kernel message log (dmesg, /proc/kmsg)
│   ├── proc/            # Processes (spawn, fork, exec, wait, kill), kernel shell, signals, users and permissions
│   ├── sched/           # Scheduler, task struct, blocking and wakeup, priorities
│   ├── sync/            # Spinlocks & Semaphores
│   ├── syscall/         # System call dispatcher & table
│   ├── time/            # PIT, the calibrated clock, one-shot timers, sleeping
│   └── bench.{c,h}      # Timed benchmarks
├── lib/                 # Kernel utility functions (kprintf, kstring, string_util)
├── mm/                  # Frames, paging and address spaces, heap, slab, copy-on-write, mmap/brk, lazily filled regions
└── net/                 # Network stack: Ethernet, ARP, IPv4, ICMP, UDP (net.c) and TCP (tcp.c)

user/                    # Programs that run in ring 3, built into /bin of the initrd
├── lib/                 # Their C library, crt0 and system call wrappers
└── *.c                  # sh, ls, cat, echo, wc, ps, kill, ... and the test programs
disk/                    # Files put on the FAT32 disk image (build/disk.img), seen at /disk
home/                    # Users' files: /home on the system disk (build/root.img, made by tools/mkroot.sh)
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

`make test-net` exchanges real UDP datagrams and TCP streams (a file sent out with `nc`, two megabytes through `tcpecho` and back) between the host and the kernel, in both directions, over the host's loopback interface; then boots on a different subnet, takes an address by DHCP, renews it twice (QEMU's packet capture must show each renewal sent to the server alone and acknowledged) and resolves a name against a DNS server in the test script.

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
