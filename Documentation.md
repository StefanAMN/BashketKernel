# BashketKernel — Curriculum & Order of Learning

Welcome to **BashketKernel**! This document serves as an ordered, structured guide for learning the concepts and features implemented throughout the 38 phases of the kernel development roadmap.

Each section breaks down a kernel subsystem, key operating system concepts, and the source files for it.

**Read the status line under each heading first.** Where a phase is marked
*partial*, the status line says which part is missing. Phase numbers are the
original ones; phases whose code was only a placeholder have been removed,
so the numbering has gaps.

---

### 1. Boot to Output & Serial Infrastructure (Phase 1)
**Status: real.** Limine with two boot entries: the system, which starts `/bin/init` (it runs `/etc/rc`, then keeps a `login` prompt on the console and starts a new one after every session, `make test-boot`), and a developer entry whose command line (`shell`) gives the console to the kernel's own shell instead (`make run-dev`, and every scripted test). `shutdown` ends every process, marks the system disk clean and switches the machine off through ACPI (the PM1 control register from the FADT, the sleep type from the `\_S5` object in the DSDT); `shutdown -r` restarts it (the FADT's reset register, or the keyboard controller). `make test-shutdown`. Framebuffer console, COM1 serial, `kprintf`.

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
  - **Booting into a system, and shutting it down:**
     - **Concepts:** a kernel command line; PID 1 and what it is for (orphans, /etc/rc, a login prompt that comes back); switching off through ACPI (the FADT's PM1 control register, the `\_S5` sleep type found in AML bytecode); resetting a PC.
     - Files to inspect: `boot_option` and the end of `_start` in `src/init/main.c`, `limine.conf`, `user/init.c`, `user/login.c`, `src/kernel/power.c`, the power part of `src/arch/x86_64/acpi.c`, `user/shutdown.c`
     - Try: `make run` (log in as root/root, then `shutdown`), `make test-boot`, `make test-shutdown`.

### 2. CPU Tables & Interrupt Handling (Phase 2)
**Status: real.** Double faults run on their own stack (IST1), and NMIs on another (IST2), each CPU having its own pair. An NMI can arrive where the CPU is in ring 0 with the user's GS base (the first and last instructions of a system call), so its entry stub (`nmi_stub` in `isr_stubs.S`) decides `swapgs` from the GS base itself rather than the saved CS; its handler only counts (it may have interrupted the holder of the output locks) and the next ordinary interrupt reports it; `/proc/cpuinfo` shows the count. `make test-nmi` sends thirty from QEMU's monitor while `/bin/syscheck` runs. PID 0, which boots the kernel, leaves the bootloader's stack for a guarded kernel stack as soon as the scheduler exists (`sched_continue_on_own_stack`).

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
**Status: real.** Bitmap frame allocator, 4-level paging, and a heap with boundary tags and free lists by size class: `kmalloc` and `kfree` take the same time whatever the number of blocks in use (tested), and freed neighbours merge at once. Freed memory is filled with 0x6b and `kmalloc` checks that what it hands out still is: a write after free panics, naming the block and the offset, and a read after free sees an obvious pattern (`heap_catches_writes_after_free`). The slab caches do the same for their objects (K1). `kheap_check` verifies the whole structure for the tests.

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
  3. **Timer IRQ (PIT) & Interrupt Acknowledgment:** (the PIT is the tick while the kernel boots; section 33 covers what replaces it)
     - **Concepts:** 8254 PIT tick frequency configuration, Local APIC and PIC EOI ordering before preemptive context switch, preventing interrupt starvation.
     - Files to inspect: `src/kernel/time/pit.h`, `src/kernel/time/pit.c`, `src/arch/x86_64/isr.c`
  4. **Round-Robin Scheduler:**
     - **Concepts:** Runqueue traversal, preemptive scheduling on timer interrupt.
     - Files to inspect: `src/kernel/sched/sched.h`, `src/kernel/sched/sched.c`

---

### 5. Userspace & System Calls (Phase 5)
**Status: real.** Programs run in ring 3 with their own address space. `syscall`/`sysret` entry, validated user pointers, per-process file descriptors, and a hardened ELF64 loader. User programs are position-independent and loaded at a random address, with a randomised stack and mmap region (ASLR).

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
**Status: real.** One tree, with nodes from the system disk (`/`), the FAT volume, and memory (`/tmp`, `/dev`, `/proc`), per-process working directories and relative paths. `truncate` and `ftruncate` set a file's length either way and `utime` its time, on memory, FAT and ext2 (`/bin/truncate`, `/bin/touch`). `stat`, `lstat` and `fstat` describe a file (type, owner, mode, size, link count, inode number, time; `/bin/stat`, `ls -l`) and an open pipe, socket or terminal. A name has at most 127 bytes and a path at most 255 (`ENAMETOOLONG` beyond); a trailing slash is not part of a name (`mkdir dir/`). Symbolic links are followed by every path lookup (at most 8 in one, `ELOOP` beyond), in the last component too except where the call says not (`readlink`, `unlink`, `rename`); in memory and on ext2 (FAT cannot store them). Hard links only on ext2. A USTAR initrd with the programs and `/etc` is loaded as a Limine module: without a system disk it fills a root kept in memory, the rescue system (`make test-rescue`), whose changes are lost at power-off.

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
**Status: real.** Two shells. The boot shell is a kernel thread with debugging commands. `/bin/sh` is a user program with pipelines, redirection of any descriptor (`2> file`, `2>&1`, applied left to right) and command lists (`;`, `&&`, `||`), `if`, `while`, `until` and `for` (nested, over several lines, with `break`, `continue` and a built-in `test` / `[`), wildcards (`*`, `?`, `[...]`), double and single quotes (parts of a word join, as in `"a b"c`), command substitution (`$(...)`), the usual text tools (`cp`, `head`, `tail`, `grep`, `sort`, `find`, `du`, reading lines of any length with `user/lib/lines.c`), and at the terminal line editing with a history and Tab completion (the arrow keys, Home, End, Ctrl-A/E/K/U/W; `user/lib/lineedit.c`), built only on system calls. Input comes from the PS/2 keyboard and the serial line at once; a machine with no PS/2 controller boots anyway, with the serial line as its only keyboard (`make test-kshell` boots one).

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
**Status: partial.** The slab allocator validates frees, honours alignment, returns empty pages, and backs open-file objects; freed objects are poisoned and checked when handed out again, so a write after free panics, naming the cache (`slab_catches_writes_after_free`). `fork` shares pages copy-on-write. `mmap` and `brk` only promise memory: a page gets its frame when it is first touched (so does the user stack, up to 8 MiB), and the calls fail when more would be promised than is free. A file can be mapped privately (`mmap` with a descriptor): each page is read from the file when first touched, and writes stay in the process. `munmap` can remove part of a mapping. Not done: shared file mappings and writing changes back, a page cache (two processes mapping one file each read their own copy), and loading executables this way. See `src/mm/region.h` and `region.c`, `src/mm/mmap.c`, `src/mm/cow.c`, and `try_promised_page` in `src/arch/x86_64/isr.c`.

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
**Status: real.** The local APIC is found through the `IA32_APIC_BASE` MSR and the I/O APIC through the ACPI MADT, and both are mapped uncached in the device region (`vmm_map_device`), not reached through the direct map. ISA IRQs are routed with the MADT's overrides; PCI devices use the line their configuration space names, which works where the firmware's MADT describes those lines (QEMU) but does not read the DSDT's `_PRT`. Only the first I/O APIC is driven. Each CPU's LAPIC timer runs its one-shot (or periodic) tick, calibrated against the PIT. A program that raises an exception is killed with the signal Unix uses for it (`SIGFPE` for a division by zero, `SIGILL` for an unknown instruction, `SIGSEGV` for bad memory accesses).

**Goal:** Local APIC timer, IOAPIC redirection table configuration.

- **Order of Learning:**
  1. **Local APIC (LAPIC):**
     - **Concepts:** Memory-mapped APIC registers (mapped uncached), LAPIC timer calibration, EOI signals.
     - Files to inspect: `src/arch/x86_64/lapic.h`, `src/arch/x86_64/lapic.c`
  2. **IOAPIC:**
     - **Concepts:** Replacing legacy 8259 PIC, routing hardware IRQs to specific APIC IDs, the MADT's I/O APIC and interrupt source override entries (`src/arch/x86_64/acpi.c`).
     - Files to inspect: `src/arch/x86_64/ioapic.h`, `src/arch/x86_64/ioapic.c`

---

### 10. SMP Multiprocessing & Spinlocks (Phase 10)
**Status: partial.** Every CPU the bootloader reports is started with its own per-CPU structure (reached through GS, with `swapgs` on every crossing between ring 3 and the kernel), task state segment and double-fault stack, and runs tasks: each has an idle task and a tick from its local APIC timer, an idle CPU is woken by an inter-processor interrupt when a task becomes runnable, tasks move between CPUs (or are tied to one), and removing a kernel mapping waits for every CPU to drop its cached copy (TLB shootdown). Shared structures are protected by ranked spinlocks and mutexes, with the lock order checked at run time. Not done: one scheduler lock and task list for all CPUs (no per-CPU queues), and all device interrupts go to the boot CPU. See `src/arch/x86_64/percpu.h`, `smp.h` and `smp.c`, `src/kernel/sync/spinlock.h` (the lock order), `mutex.h`, and `src/kernel/sched/sched.c`.

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
**Status: real.** virtio-blk is a real driver (one virtqueue, interrupt-driven, up to eight requests with the device at once); `/dev/vda` is the raw disk. A write-through cache of 1024 sectors (`src/drivers/block/bcache.c`) sits between the filesystem and the driver. The FAT32 volume on it is mounted at `/disk`, readable and writable: files and directories can be created, written, appended, truncated, renamed, moved and removed, programs can be run from it, and everything survives a reboot. Long file names are read and written (ASCII; up to 127 characters), with invented short aliases; names keep their case and are unique without regard to it. A directory is read from the disk when something first looks inside it. Entries carry the date and time from the RTC. `fsck.fat` accepts what the kernel writes. Limits: no journal, writes are not delayed or batched, names outside ASCII are shown by their short alias, looking a name up is case-sensitive, and FAT stores no owners or permissions. See `src/fs/fat32.c` (the "Names" and "Directory slots" sections for long names), `vfs_rename` and `vfs_load_dir` in `src/fs/vfs.c`, and `tools/mkfat.py`, which writes the same format from the host.

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
**Status: partial.** `fork` (copy-on-write; it refuses rather than leave no memory for the copies), `execve`, `waitpid` (with `WNOHANG` and `WUNTRACED`), `kill` (a negative number signals a process group), exit statuses, `dup`, `pipe2` and close-on-exec. A faulting process is killed without affecting the kernel. Signals can end a process, stop and continue it (`SIGSTOP`, `SIGTSTP`, `SIGCONT`), be ignored, or run a handler in the program (`signal`, `sigaction`), with the interrupted state saved on the user stack and restored by `sigreturn`. They can be blocked (`sigprocmask`; a handler's own signal is held back while it runs), a handler set with `SA_RESTART` restarts the `read`, `write`, `waitpid` or `accept` it interrupted, and `SIGCHLD` tells a parent that a child has exited or stopped. Signals take effect at safe points. `/bin/init` adopts processes whose parent has died and waits for them. Process groups and a terminal foreground group give the user shell job control (`&`, Ctrl-Z, `jobs`, `fg`, `bg`). Not done: there are no sessions, and a background job that reads the terminal is not stopped. See `src/kernel/proc/process.c`, `signal.h` and `signal.c`, the stop and adoption code in `src/kernel/sched/sched.c`, `user/init.c`, and the jobs part of `user/sh.c`.

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
**Status: real.** PCI scan (function 0 of each device), configuration space access, capability list, shared interrupt lines. The virtio drivers reach their devices through either of virtio's register interfaces: the modern one (registers in memory, found through the capability list, 64-bit feature negotiation) when the device offers it, the legacy I/O port block otherwise; `make check` runs the tests with devices that offer both, only the legacy and only the modern one. virtio-net is a real driver: receive and transmit virtqueues, interrupt-driven, MAC address from the device. Interrupts are the shared PCI line, not MSI-X.

**Goal:** PCI bus enumeration, VirtIO Net driver (Ethernet frame transmit/receive).

- **Order of Learning:**
  1. **PCI Bus Subsystem:**
     - **Concepts:** Scanning PCI buses (0–255), reading vendor/device IDs via config ports `0xCF8/0xCFC`.
     - Files to inspect: `src/drivers/bus/pci.h`, `src/drivers/bus/pci.c`
  2. **Finding a virtio device's registers:**
     - **Concepts:** the PCI capability list; base address registers (I/O or memory, 32 or 64 bits); mapping device memory uncached, outside the direct map; why nothing a device says about itself is believed; the handshake (reset, acknowledge, features, FEATURES_OK, queues, DRIVER_OK) and what differs between the legacy and modern interfaces.
     - Files to inspect: `src/drivers/virtio/virtio_pci.h`, `src/drivers/virtio/virtio_pci.c`, `vmm_map_device` in `src/mm/vmm.c`, `tests/test_virtio.c`
     - Try: `make test VIRTIO=legacy`, `make test VIRTIO=modern`.
  3. **VirtIO Net Interface Card (NIC):**
     - **Concepts:** Rx/Tx virtqueue initialization, MAC address retrieval, raw Ethernet frame transmission.
     - Files to inspect: `src/drivers/net/virtio_net.h`, `src/drivers/net/virtio_net.c`

---

### 16. Userland Application Infrastructure (Phase 16)
**Status: real.** `user/*.c` are built into `/bin` with a small C library (`user/lib`): `sh`, `ls`, `cat`, `echo`, `wc`, `hello`, `fault`, `syscheck`.

**Goal:** Loading and executing standalone ring-3 binaries from filesystem.

- **Order of Learning:**
  1. **Userland Execution Manager:**
     - **Concepts:** Spawning ring-3 tasks from ELF binaries located on disk or initrd.
     - Files to inspect: `src/kernel/proc/user_apps.h`, `src/kernel/proc/user_apps.c`

---

### 17. Real-Time Clock (RTC) and the Time of Day (Phase 17, PLAN F1)
**Status: real.** Reads date and time from CMOS, under a lock (the index and data ports are shared by every CPU) and twice until two reads agree (the clock updates its registers one at a time), in BCD or binary and 12- or 24-hour form, whichever the clock keeps (`rtc_decode`, tested for each). The kernel reads it once, as soon as the nanosecond clock is calibrated, and keeps the time of day from then on as that reading plus the nanosecond clock (`walltime.c`): programs read it with `time_ns()` and root sets it with `set_time()` (`/bin/date`, `date -s`); the CMOS chip itself is not written. File times on ext2 and FAT come from it, and the VFS keeps them as Unix seconds, which `ls -l` prints. UTC only: no time zones, no leap seconds; the CMOS century is assumed to be 2000, and the kernel accepts times from 1980 to 2105.

**Goal:** Reading CMOS Real-Time Clock registers for system date and time, and keeping the time of day without reading them again.

- **Order of Learning:**
  1. **CMOS Real-Time Clock:**
     - **Concepts:** Reading CMOS ports `0x70/0x71`, BCD format decoding to UTC time struct.
     - Files to inspect: `src/drivers/char/rtc.h`, `src/drivers/char/rtc.c`
  2. **The time of day:**
     - **Concepts:** an offset from a monotonic clock; days and dates in the proleptic Gregorian calendar (Hinnant's algorithms, in `lib/time/date.c`, compiled into the kernel and the user library).
     - Files to inspect: `src/kernel/time/walltime.h`, `src/kernel/time/walltime.c`, `user/date.c`, `tests/test_rtc.c`

---

### 18. Environment Variables (Phase 18)
**Status: real.** Each process has its own environment in its own memory, carried across `fork` and `execve`. The C library has `getenv`/`setenv`; `/bin/sh` has `export`, `$NAME`, `$?` and `PATH` lookup. 

**Goal:** Per-process environment variable management (`getenv`, `setenv`, `env`).

- **Order of Learning:**
  1. **Process Environment Storage:**
     - **Concepts:** Key-value string array storage, inheritance across `fork`/`exec`.
     - Files to inspect: `src/kernel/proc/process.c` (`build_user_stack`), `user/lib/ulib.c` (`getenv`, `setenv`), `user/sh.c` (`export`)

---

### 19. Inter-Process Communication Pipes, and `poll` (Phase 19, PLAN F2)
**Status: real.** Blocking pipes behind file descriptors, with end-of-file and broken-pipe handling. Used by `/bin/sh` for `a | b`. `poll` waits on up to 64 descriptors at once: pipes, the terminal (a complete line in line mode, any key in raw mode), UDP and TCP sockets (a listener is readable when a connection waits for `accept`), the mouse; files and other devices are always ready. A poller is woken by any change anywhere and looks at its own descriptors again (a generation counter keeps a change during that look from being lost); simple, and fine for a system with a few programs. `/bin/nc` uses it to carry both directions in one process. Not done: `select`, edge-triggered or `epoll`-style interfaces, and readiness of regular files on a disk (always ready, as on Unix).

**Goal:** POSIX Anonymous Pipes (`pipe`), blocking read/write ring buffers; waiting on several descriptors at once.

- **Order of Learning:**
  1. **IPC Ring-Buffer Pipes:**
     - **Concepts:** Circular byte buffer, task sleeping on empty/full buffer, pipe EOF handling.
     - Files to inspect: `src/fs/pipe.h`, `src/fs/pipe.c`
  2. **poll:**
     - **Concepts:** readiness per kind of file, one wake-up channel for all pollers, the check-then-sleep race and the counter that closes it.
     - Files to inspect: `src/fs/poll.h`, `src/fs/poll.c`, `pipe_poll`, `tty_poll`, `tcp_poll`, `user/nc.c`, `tests/test_poll.c`

---

### 20. Terminal ANSI Escape Sequence Parser (Phase 20)
**Status: real.** The console interprets ANSI escape sequences: 16 text and background colours, bold, cursor positioning and relative movement, erasing the screen or a line. `/bin/clear` and `/bin/colors` use them. Unsupported sequences are swallowed. See `src/drivers/console/ansi.c` (parser) and `src/drivers/console/console.c` (what the screen does with it).

**Goal:** ANSI escape sequence handling for text coloring and cursor movement.

- **Order of Learning:**
  1. **ANSI Sequence Parser:**
     - **Concepts:** State machine parsing for CSI sequences (e.g. `\033[31m` for red text).
     - Files to inspect: `src/drivers/console/ansi.h`, `src/drivers/console/ansi.c`

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
**Status: real.** `/proc/version`, `uptime`, `meminfo`, `tasks`, `cpuinfo`, `kmsg` and `mounts` are generated from live kernel state on every read. `/proc/<pid>/` has a directory per task with `status` (name, state, user and group, parent, process group, heap and mapped memory) and `cmdline` (the arguments it was started with, NUL-separated); `/proc/self` is a link to the reader's own. The VFS keeps the whole tree in memory, so the directories are kept in step with the task list by a `refresh` operation on `/proc`, run on every lookup and listing there; the nodes hold only a pid and each read looks the task up under the scheduler lock, so a reaped task reads as ENOENT rather than through a freed pointer. Nothing under `/proc` can be removed. `ps` reads `/proc/<pid>/status` (`tests/test_procfs.c`, syscheck's `check_proc`).

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

### 28. Dynamic Executable Linking (Phase 28)
**Status: real.** Static position-independent executables: the loader places them at a chosen or random address and applies their relative relocations, refusing any that would write outside the image. No shared libraries, no symbol resolution, no dynamic linker.

**Goal:** Parsing dynamic ELF section headers (`.dynamic`, `.got`, `.plt`), runtime symbol resolution.

- **Order of Learning:**
  1. **Dynamic ELF Resolver:**
     - **Concepts:** Global Offset Table (GOT) relocation, Procedure Linkage Table (PLT) stubs.
     - Files to inspect: `src/fs/elf_dyn.h`, `src/fs/elf_dyn.c`

---

### 29. UDP and TCP Sockets over IPv4, DHCP, DNS (Phase 29)
**Status: partial.** Ethernet, ARP (answers older than a minute are asked for again), IPv4, ICMP echo, UDP (64 KiB queued per socket) and TCP, with sockets as file descriptors (`socket`, `bind`, `sendto`, `recvfrom`; `connect`, `listen`, `accept`, `shutdown`, and `read`/`write` on a connection) and a `ping` call. TCP has the handshake, retransmission with back-off, flow control by the receiver's window, orderly close and reset; it has no congestion control, does not keep out-of-order segments, and uses a fixed initial retransmission time (see `src/net/tcp.h`). The interface starts with the addresses QEMU's user-mode network expects (10.0.2.15, gateway 10.0.2.2); `/bin/dhcp` replaces them with what a DHCP server offers (the lease is not renewed), and names are resolved by a DNS client in the C library (`host`, `ping NAME`; A records only, no cache). No IPv6, no fragmentation. See `src/net/net.c`, `src/net/tcp.h` and `tcp.c`, `user/nc.c`, `user/tcpecho.c`, `user/dhcp.c`, `user/host.c`, and the DNS functions in `user/lib/ulib.c`.

**Goal:** Network socket layer (`socket`, `bind`, `sendto`, `recvfrom`), IPv4/UDP packet handling.

- **Order of Learning:**
  1. **Network Sockets:**
     - **Concepts:** Socket descriptor table, UDP packet header building, IP checksum computation.
     - Files to inspect: `src/net/socket.h`, `src/net/socket.c`

---

### 31. User Account Authentication (Phase 31)
**Status: partial.** Processes run as a user and a group (numbers; root is 0). Files have an owner, a group and Unix permission bits (read, write and execute for owner, group and others), checked on open, create, remove, `execve` and `chmod`, and every directory a path passes through needs search (execute) permission; new files get 0666 less the process's `umask`. A set-uid file runs with its owner's rights while the process keeps its real user id (`getuid` / `geteuid`); the bit is dropped when a non-root user writes to the file or the file changes owner. `/bin/login`, `/bin/su` and `/bin/passwd` (the last two set-uid root) check passwords stored in `/etc/passwd` as salted PBKDF2-HMAC-SHA256 hashes. A process may also be in up to 16 supplementary groups (`setgroups`, root only, and `getgroups`), set at login from `/etc/group`, kept across `fork` and `execve`, and counted by every permission check; `id` lists them, and an owner may give a file to any group they are in. Shared memory segments belong to their creator. Not done: set-gid and sticky bits; files on the FAT volume all belong to root (FAT cannot store owners); `/etc/passwd` lives in the initrd, so a changed password lasts until reboot. See `src/kernel/proc/auth.h` and `auth.c`, `user/lib/password.h` and `password.c`, `user/login.c`, `user/su.c`, `user/passwd.c`, `tools/mkpasswd.py`.

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

### 33. High-Resolution and One-Shot Timers (Phase 33)
**Status: real.** The timestamp counter is calibrated against the PIT at boot, giving a nanosecond clock (`clock_ns`). The kernel then stops the periodic tick: the tick count is read off the clock, and each CPU's local APIC timer is set to fire once, when that CPU next has something to do (10 ms on while it runs a task, a sleeper's deadline, output to show). An idle CPU is not interrupted at all (four idle CPUs: 7 timer interrupts in three seconds instead of 1200; `/proc/timers` counts them), and `sleep` and `nanosleep` end when asked, not at the next tick. On a CPU without a timestamp counter the kernel keeps the 100 Hz tick; that fallback is run by `make check` (`-cpu qemu64,-tsc`).

**Goal:** understand the two ways a kernel can keep time (count interrupts, or read a counter and ask for an interrupt only when one is needed) and what each costs.

- **Order of Learning:**
  1. **A clock that is read, not counted:**
     - **Concepts:** the timestamp counter, calibrating one clock against another, why 64-bit arithmetic has to be split to avoid overflow.
     - Files to inspect: `src/kernel/time/hrtimer.h`, `src/kernel/time/hrtimer.c`
  2. **One-shot timers:**
     - **Concepts:** what a CPU needs a timer interrupt for (sharing the CPU, waking sleepers, showing output) and that an idle CPU needs none; arming for the earliest deadline; why firing early is harmless and firing late is not.
     - Files to inspect: `src/kernel/time/timer.h`, `src/kernel/time/timer.c`, `lapic_timer_arm` in `src/arch/x86_64/lapic.c`
  3. **Who owns a deadline:**
     - **Concepts:** each CPU answers for the tasks that went to sleep on it; the idle loop as the one place every CPU passes on its way to halting.
     - Files to inspect: `sleep_on`, `sched_wake_sleepers` and `sched_idle_loop` in `src/kernel/sched/sched.c`
  4. **Nothing may poll:**
     - **Concepts:** a thread that wakes "every so often, in case" keeps a CPU awake for ever; sleeping until there is work (`sched_wait_for_reapable`, `tcpd` with no connections).
  5. **The fallback, and testing it:** `make test CPU=qemu64,-tsc`.

---

### 34. VFS Mount Manager (Phase 34)
**Status: real.** Mount points: `/dev`, `/proc`, `/tmp` and the FAT32 volume at `/disk` sit on a directory the kernel puts in place (`vfs_mount_dir`). On a directory read from a disk, the disk's own entry of that name is hidden, not changed, and comes back when the mount point is taken away; a mount point cannot be removed or renamed (`EBUSY`). A record of which source provides which part of the tree is shown by `mount` and `/proc/mounts`. Path lookup does not use it: every source attaches its nodes into the one tree. Only an empty memory directory can be unmounted; volumes stay mounted.

**Goal:** Path mount points (`vfs_mount`), mount table management, shell `mount` command.

- **Order of Learning:**
  1. **Mount points:**
     - **Concepts:** putting one tree on top of a directory of another; hiding the covered directory rather than changing it; why a mount point must not be removed while mounted; what happens when the directory around it is read from its disk again.
     - Files to inspect: `vfs_mount_dir`, `vfs_unmount_dir` in `src/fs/vfs.h` and `src/fs/vfs.c`; `mount_points_cover_a_disk_directory` in `tests/test_ext2.c`
  2. **Mount table:**
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
**Status: real, and small on purpose.** Two primitives, both in use: SHA-256 (passwords are hashed with PBKDF2-HMAC-SHA-256 by `/bin/login` and `/bin/passwd`, and the random generator is rekeyed through it) and ChaCha20 (the random generator's expansion step). Each is checked against published test vectors (FIPS 180-2's for SHA-256, RFC 8439's for ChaCha20), not against itself. The other fourteen algorithms that used to be here (AES and its modes, SHA-1, SHA-512, MD5, BLAKE2s, SipHash, Poly1305, HKDF, Ed25519, CRC32, ...) were deleted in PLAN.md track E: nothing used them and nothing checked them against a reference.

**Goal:** The hash and the stream cipher the kernel's own security rests on, correct by outside evidence.

- **Order of Learning:**
  1. **SHA-256:** the message schedule, the 64 rounds, and the padding that may need a second block.
     - Files to inspect: `lib/crypto/sha256.c` (compiled into the kernel and into the user library), `user/lib/password.c` (HMAC and PBKDF2 on top of it), `tests/test_crypto.c`
  2. **ChaCha20:** the quarter round, twenty rounds, and how `src/drivers/char/random.c` uses the keystream (fast key erasure).
     - Files to inspect: `lib/crypto/chacha20.c`, `src/drivers/char/random.c`, `tests/test_dev.c`

---

### 37. Virtual Hardware Devices (/dev) (Phase 37)
**Status: real.** `/dev/null`, `/dev/zero`, `/dev/random` and `/dev/console` are device nodes with their own read/write functions. `/dev/random` gives output of the kernel's generator: ChaCha20 with the key replaced after every request, seeded from RDSEED or RDRAND where the CPU has them and otherwise only from interrupt timing, which is weak right after boot (the boot log says which). See `src/drivers/char/random.h`.

**Goal:** Virtual character devices (`/dev/null`, `/dev/zero`, `/dev/random`), the kernel random number generator (`get_random_bytes`).

- **Order of Learning:**
  1. **Virtual Device Tree:**
     - **Concepts:** Mount `/dev` nodes (`/dev/null`, `/dev/zero`, `/dev/random`), entropy and expansion in a random number generator (`src/drivers/char/random.c`).
     - Files to inspect: `src/drivers/char/dev.h`, `src/drivers/char/dev.c`

---

### 38. Kernel Performance Benchmark Suite (Phase 38)
**Status: real.** `bench` times memcpy, memset, heap and frame allocation for 100 ms each and prints rates. Timer resolution limits accuracy to a few percent.

**Goal:** CPU MIPS calculation & memory throughput diagnostic suite.

- **Order of Learning:**
  1. **Benchmark Suite:**
     - **Concepts:** Memory copy benchmark, CPU loop benchmark (`bench_run_all`).
     - Files to inspect: `src/kernel/bench.h`, `src/kernel/bench.c`

### 39. A Filesystem with Owners: ext2
**Status: partial.** The system disk: an ext2 volume on the second virtio disk is the root of the file tree, with the programs in `/bin`, `/etc` and the users' home directories; files have owners, groups, permission bits and times that are stored on the disk and survive a reboot (a changed password included). Reading and writing (through the doubly indirect block), create, remove, `mkdir`, rename and move, truncation; `chmod` and `chown` are recorded; symbolic links (short ones inside the inode, longer ones in a block) and hard links, made with `ln` and `ln -s`, accepted by `e2fsck`, and links made on the host by `mke2fs` are followed. Built on the host with `mke2fs` and `debugfs` (`tools/mkroot.sh`); `e2fsck` finds nothing to fix after the kernel has written to it, and images damaged on purpose do not bring the kernel down. The volume carries a clean mark (the superblock's state) only while it is not in use: it is taken away at mount and put back by `shutdown`. When it is missing at mount, the last session ended in a crash, and the whole volume is checked: what an interrupted write can leave behind is put right (`make test-crash`). Not done: device files, the triply indirect block, a journal.

**Goal:** see how a Unix filesystem stores what FAT cannot: one record (inode) per file holding owner, mode, times and block numbers, with names kept separately in directories.

1. **Layout**: superblock, group descriptors, bitmaps, inode table.
     - **Concepts:** block groups; free-space bitmaps; why counts are kept in two places.
     - Files to inspect: the comment at the top of `src/fs/ext2.c`, `ext2_parse_superblock`, `alloc_block`, `alloc_inode`
2. **A file's blocks**: direct, indirect and doubly indirect block numbers.
     - **Concepts:** a tree of block numbers; holes; why a large file costs a few extra blocks.
     - Files to inspect: `file_block`, `free_tree` in `src/fs/ext2.c`
3. **Directories**: variable-length records that are split and merged.
     - Files to inspect: `dir_add`, `dir_change`, `ext2_dir_load`
4. **Order of writes**: inode before name on creation, name before inode on removal.
     - **Concepts:** what a crash can leave behind, and why `e2fsck` can repair it.
     - Files to inspect: `ext2_dir_create`, `ext2_dir_remove`, `ext2_dir_rename`
5. **After a crash**: what can be wrong, and how the mount-time check puts it right.
     - **Concepts:** reachability from the root; why "allocated but unreferenced" is the only damage ordered writes allow; what a journal would add (nothing half-written at all).
     - Files to inspect: `ext2_check` in `src/fs/ext2.c`, `tools/crash_test.py`, `user/fsstress.c`
     - **Concepts:** the clean mark: how a volume says "I was put away properly", why it is taken away at mount, and who puts it back (`ext2_mark_clean`, called by `system_shutdown` in `src/kernel/power.c`).
6. **Checking against other implementations**: `tools/mkroot.sh` (the image is made by `mke2fs`), `make test-home` (what the kernel writes is judged by `e2fsck`), `tools/corrupt_ext2.py`.

### 40. The Mouse and the Screen as Devices
**Status: partial.** A PS/2 mouse driver (the keyboard controller's second port, IRQ 12; three-byte packets decoded with resynchronisation) read by programs from `/dev/mouse`, and the screen as a file of pixels, `/dev/fb0` (`fb_info()` gives its geometry). A program that opens the screen for writing has it to itself: the console keeps its text up to date in RAM and on serial and repaints when the program closes the device or dies. `/bin/paint` draws with the mouse; `make test-paint` moves and clicks the mouse through QEMU's monitor and checks screenshots. Not done: mapping the screen into a program (`mmap`), a wheel, waiting for the mouse and the keyboard at once (there is no `poll`), and any window system.

**Goal:** see that "graphics" is two files, one read and one written, and that sharing a piece of hardware between the kernel and a program needs an owner.

1. **A second device on the keyboard controller.**
     - **Concepts:** the controller's auxiliary port and how the status register says whose byte is waiting; one data port, so one lock; commands that are acknowledged; giving up when nothing answers.
     - Files to inspect: `ps2_aux_enable`, `ps2_aux_command`, `ps2_read_aux` in `src/drivers/input/keyboard.c`
2. **A packet protocol with no framing.**
     - **Concepts:** three bytes with only one bit to say where a packet starts; 9-bit signed numbers; what to do when the reader is slower than the mouse (add movements up, never lose the buttons' last state).
     - Files to inspect: `src/drivers/input/mouse.h`, `src/drivers/input/mouse.c`, `tests/test_mouse.c` (the tests play the mouse)
3. **Who has the screen.**
     - **Concepts:** the console's copy of the screen in RAM; handing video memory to a program and taking it back; tying that to the open file so that it ends when the program does, however it ends.
     - Files to inspect: `console_graphics_begin` in `src/drivers/console/console.c`, the `opened`/`closed` hooks in `src/fs/vfs.h` and `src/fs/file.c`, `fb_ops` in `src/drivers/char/dev.c`
4. **A program that draws.**
     - **Concepts:** the pointer is drawn by the program; keeping a copy of the picture to take the pointer off again; Bresenham's line.
     - Files to inspect: `user/paint.c`
     - Try: `make run`, then `exec /bin/paint` in the QEMU window (click inside it first; right button to leave).
5. **Testing something you look at:** `make test-paint`, `tools/check_ppm.py`, and the `@mouse`, `@button` and `@screendump` commands of `tools/qemu_shell.py`.

### 41. Swap: More Memory than the Machine Has
**Status: partial.** With a swap disk attached (one made by the host's `mkswap`; the kernel checks its header), a process that page-faults while memory is short pushes its own least recently used pages out to the disk (the clock algorithm, on the accessed bit) and they are read back when touched; `mmap` and `brk` may then promise more memory than the machine has. A swapped page is a not-present page-table entry holding its slot; `fork` shares the slot and each side reads its own copy. `/bin/swapper 80` runs on a 64 MiB machine (`make test-swap`); `/proc/meminfo` shows swap totals and counts. Limits, by design: a process only gives up its own pages, and only at a fault from user mode (never while it is in a system call, and never another process's); frames still shared after a `fork` stay; a page read back is written again in full when it next goes out; one swap device; swap is attached only where asked (`tools/qemu_shell.py --swap-disk`).

**Goal:** understand what a page-table entry can say besides "here is the frame", and why deciding *whose* page to take is harder than taking it.

1. **A page that is somewhere else.**
     - **Concepts:** the CPU ignores every bit of a not-present entry but one, so the kernel can keep a note there; the slot number where the frame address was; permissions kept for the return.
     - Files to inspect: `VMM_SWAPPED`, `vmm_pte_swapped` in `src/mm/vmm.h`; `vmm_swap_pick`, `vmm_swap_install` in `src/mm/vmm.c`
2. **Choosing the victim: the clock.**
     - **Concepts:** the accessed bit; a second chance; why clearing the bit needs an `invlpg`; a hand that remembers where it stopped.
     - Files to inspect: `swap_scan` in `src/mm/vmm.c`, `tests/test_swap.c` (`recently_used_pages_are_spared`)
3. **Out and back.**
     - **Concepts:** take the page away first, then write it; one sleeping lock so that a fault on a page in transit waits; what to undo when the disk fails.
     - Files to inspect: `swap_out`, `swap_in` in `src/mm/swap.c`
4. **Whose page, and when.**
     - **Concepts:** the kernel checks a buffer and uses it later; local against global replacement; why a fault from user mode is the one moment a process's own pages are safe to take; pages shared after `fork`.
     - Files to inspect: the comment at the top of `src/mm/swap.h`, `try_swap` in `src/arch/x86_64/isr.c`, `process_populate` in `src/kernel/proc/process.c`, and the design notes in `PLAN.md` (C4)
5. **fork, unmap, exit.** Every place an entry is copied or dropped must know the new kind: `vmm_clone_user_space`, `free_table`, `user_unmap_range`, `region_fault`, `process_grow_stack`.
6. **Try:** `make test-swap`; then `tools/qemu_shell.py --mem 64M --swap-disk build/swap.img 'exec /bin/swapper 80' 'cat /proc/meminfo'`, and the same without `--swap-disk`.
