# BashketKernel — Plan

This document lists what to build next. Tracks A, B and C are done (items
are marked as they land). Track D makes the system live on its disk instead
of in memory; track E documents every function and removes code nothing
uses. Everything not marked done is still a proposal.

The previous plan (the repair plan that took the tree from mostly-mock to
real) is finished and remains in git: `git show e04ac04:PLAN.md`. The
original learning roadmap is at `git show 0c3bd4d:plan.md`.

## Where the kernel stands

Implemented and covered by `make check` (the documentation check, unit
tests on QEMU's default CPU, on `-cpu max` and without a timestamp counter,
on 1 to 8 CPUs, and scenario targets for the disks, the network, the
editor, login, shutdown, power cuts, swap, the mouse and fuzzing):

- Boot through Limine into `/bin/init` and a login prompt, from an ext2
  system disk (a rescue system in memory without one); clean shutdown and
  restart through ACPI.
- Every CPU scheduling, with per-CPU data, ranked spinlocks checked at run
  time, TLB shootdown and one-shot local APIC timers.
- Memory: frames, page tables, heap, slab, copy-on-write `fork`, lazy and
  file-backed mappings, shared memory, swap.
- Processes: signals with handlers and masks, process groups and job
  control, users, groups, permissions and set-uid programs, ASLR, SMEP,
  SMAP and UMIP where the CPU has them.
- Files: ext2 (the root) and FAT32 on virtio disks, a sector cache, mount
  points, `/proc`, `/dev` (including the screen and the mouse), pipes.
- Network: virtio-net, ARP, IPv4, ICMP, UDP, TCP, DHCP and DNS.
- A terminal with line and raw modes, a user-space shell with pipelines and
  jobs, an editor, a paint program and about forty other programs.
- Every function has a comment saying what it does (track E).

## How to read the proposals

Each proposal says what it adds, why it is worth doing here, what it
depends on, roughly how large it is (S: a day or less; M: a few days;
L: a week or more, at the pace of the work so far), and the check that would
prove it. They are grouped into three tracks that can advance independently.

---

## Track A — Make the core sound for more than one CPU

This is the largest structural gap. Every CPU is started today but only
CPU 0 runs tasks, because "interrupts off" is used as the lock almost
everywhere. Nothing else in this plan is blocked by it, but everything
written before it is done adds to the work.

### A1. Per-CPU data and a task state segment per CPU (M) — done

- A `cpu_t` per CPU reached through the GS base (`swapgs` on entry from
  ring 3), holding the current task, the kernel stack pointer for system
  calls (today the globals `syscall_kernel_rsp` and `syscall_user_rsp`), and
  the CPU's TSS.
- One GDT TSS descriptor and one IST stack per CPU, so that the parked CPUs
  can take exceptions and interrupts at all.
- **Depends on:** nothing.
- **Check:** all existing targets pass with one CPU; with four, each CPU can
  take a deliberate breakpoint exception and report its own id.

### A2. Real locks in place of interrupt masking (L) — done

Done as described, with these differences. The check is in every build, not
a debug build, and stops the kernel at the next timer tick. The VFS turned
out to have no protection at all and got a (re-entrant) mutex. The other
CPUs do not run kernel threads yet (that is A3), so the cross-CPU test runs
a function on them directly with `smp_call`, which uses a new wake-up
inter-processor interrupt. That test is known to miss very narrow races
(see CLAUDE.md, "Check that a concurrency test can fail"). Position and
flags of an open file shared after `fork` are still not locked.

- Audit every `irq_save` critical section and give each shared structure a
  spinlock taken with interrupts off: the run queue and task list, wait
  channels, pipes, semaphores, the terminal, the console, the network
  stack's tables, sockets, shared memory segments, the FAT volume state, the
  virtqueues, the slab caches, the mount and descriptor tables.
- Write down the lock order (today: heap or slab, then VMM, then PMM;
  `kprintf` leaf) and add a debug build that checks it at run time.
- **Depends on:** A1.
- **Check:** a lock-order checker reports nothing across `make check`; the
  unit tests pass with four CPUs while a second CPU runs a kernel thread
  that hammers each subsystem.

### A3. Scheduling on every CPU (L) — done

Done, with these differences from the list below. There is one task list
and one scheduler lock for all CPUs rather than a run queue per CPU: every
CPU takes the next runnable task it is allowed to run, so load spreads by
itself, at the price of a contended lock and no cache affinity. The boot CPU
kept the PIT as its tick (and as the system's clock) and the other CPUs used
periodic local APIC timers, until C4's one-shot timer replaced both. TLB shootdown is done for the
kernel half only, where it is needed: a process's own mappings are only ever
live on the one CPU running it. Device interrupts all still arrive on the
boot CPU. Measured under QEMU: four CPU-bound tasks on four CPUs finish in
about 1.3 times the time of one (a threefold speed-up).

- A run queue per CPU, an idle task per CPU, the timer interrupt on each
  CPU's local APIC (which also removes the PIT's 10 ms granularity for
  wake-ups), and a reschedule inter-processor interrupt.
- TLB shootdown: unmapping or write-protecting a page in an address space
  that is active on another CPU must interrupt that CPU.
- FPU state stays per task and is saved by the CPU that switches it out.
- Simple load balancing: a new task goes to the least loaded CPU.
- **Depends on:** A1, A2.
- **Check:** `/bin/fptest`, `/bin/syscheck`, the fuzzer and the low-memory
  scenarios pass on 2, 4 and 8 CPUs; a CPU-bound workload with N processes
  finishes close to N times faster on N CPUs; `/proc/tasks` shows which CPU
  each task last ran on.

### A4. Narrow the SMAP windows (S) — done

Done differently from the proposal below: instead of passing a descriptor of
the user buffer down to the final copy, `read` and `write` copy through one
page of kernel memory that belongs to the task, so nothing below the system
call ever receives a user pointer. Simpler, at the cost of copying the data
twice. A test device checks both properties (kernel pointer, window closed).

- The user access window is open for the whole of `file_read`/`file_write`.
  Pass "this is a user buffer" down to the final copy (a small `uio`-style
  descriptor) so that the window covers only the copy.
- **Depends on:** nothing.
- **Check:** unit tests and fuzzing on `-cpu max`; a deliberately misplaced
  user dereference inside the pipe or FAT code faults.

---

## Track B — Make it a more complete Unix

### B1. TCP (L) — done

Done as described (`src/net/tcp.c`; the limits are listed at the top of
`tcp.h`). In addition to the list below there is `shutdown` for the write
half, which `nc` needs. Checked over loopback in the kernel (including with
one segment in seven thrown away, and with a peer that stops answering),
from ring 3, and against the host: an 88 KB file out through `nc`, 2 MiB
through `tcpecho` and back.

- Connections over the existing IPv4 layer: three-way handshake, sequence
  and acknowledgement numbers, retransmission with a timer, a fixed receive
  window, orderly close and reset. No congestion control beyond a fixed
  window to begin with; say so.
- Sockets: `connect`, `listen`, `accept`, and `read`/`write` on a connected
  socket, as file descriptors.
- **Depends on:** nothing (the timer wheel it needs is small).
- **Check:** `tools/test_net.py` gains a TCP exchange with the host in both
  directions through QEMU's port forwarding; a transfer of several megabytes
  arrives intact with artificially dropped segments; `/bin/nc`-style client
  and server programs.

### B2. DHCP and DNS (M) — done

Done as user programs (`dhcp`, `host`, and name support in `ping`) over a
kernel whose address is now a setting (`netconfig`) and which can work
unconfigured: broadcast from 0.0.0.0, accept whatever reaches its card. The
kernel still boots with QEMU's usual addresses, so nothing depends on a
DHCP server being there; `dhcp` replaces them. Not done: lease renewal, a
resolver cache, record types other than A. The DNS check uses a server in
the test script; against a real resolver it was only tried by hand.

- A DHCP client so that the address is no longer fixed at 10.0.2.15, and a
  resolver for names (`ping example.com`). Both are user programs plus a
  system call to set the interface address.
- **Depends on:** nothing for DHCP; DNS can only be tested against QEMU's
  built-in forwarder, which needs the host to have a resolver.
- **Check:** boot with no configured address, obtain one from QEMU's DHCP
  server, ping the gateway by the address it gave.

### B3. Processes: the missing pieces (M) — done (booting into init: D3)

Done: signal masks, `sigaction` with `SA_RESTART`, `SIGCHLD`; `WNOHANG` and
`WUNTRACED`, `dup`, `pipe2`, close-on-exec; stop and continue signals;
process groups, a foreground group for the terminal, and job control in the
shell; `/bin/init`, to which orphans are handed and which waits for them.

Booting into init came later, as D3. Absent: sessions,
and stopping a background job that reads the terminal (`SIGTTIN`).

- Signal masks and `sigaction`, `SIGCHLD`, restart of interrupted calls.
- Re-parenting of orphans to an init process, and booting into `/bin/init`
  which starts the shell (the kernel shell becomes a debug monitor reached
  by a key or a boot option).
- Job control in the shell: `&`, `fg`, `bg`, Ctrl-Z, process groups and a
  foreground group for the terminal.
- `waitpid` options (`WNOHANG`), `pipe2`, `dup`, close-on-exec.
- **Depends on:** nothing.
- **Check:** new sections in `/bin/syscheck`; a scripted shell session with
  background jobs.

### B4. Memory: lazy and file-backed mappings (M) — done, without the page cache

Done: `mmap` and `brk` record a region and give each page its frame on first
touch; the pages promised and not yet touched are counted, and a call that
would promise more than is free fails. Private file mappings read each page
when it is touched (ten disk requests instead of 442 for one byte of a
100 KiB file). The kernel gives a program's buffer its frames before using
it, so its own accesses never fault.

Not done: executables are still copied in whole by the loader, because
sharing their pages between processes needs a page cache, and that is the
larger half of this item. Shared file mappings and write-back likewise.
The promise limit does not cover fork or copy-on-write.

- Demand-zero `mmap` and `brk` (pages appear on first touch), with an
  accounting limit so that overcommit is a decision rather than an accident.
- `mmap` of files, read-only and private first; then use it to load
  executables without copying them (the page cache this implies is the real
  work).
- **Depends on:** nothing; interacts with A2 and A3 (TLB shootdown) if done
  after them.
- **Check:** `/bin/stress` variants; a program mapped from `/disk` whose
  untouched pages are never read from the disk.

### B5. Permissions: the rest (M) — done, with one group per process

Done: a group per process and per file with the group's permission digit,
the execute bit (checked by `execve`), set-uid programs with real and
effective user ids, `umask`, `su` and `passwd`, salted PBKDF2 password
hashes (checked against Python's implementation: `tools/mkpasswd.py` makes
the lines `/bin/login` accepts), and owners for shared memory segments.

Not done: supplementary groups, the execute ("search") bit on directories,
set-gid and sticky bits. Owners for files on `/disk` wait for a filesystem
that can store them (C2), and a password changed with `passwd` is lost at
reboot for the same reason: `/etc` is in the initrd.

- Groups, the execute bit, set-uid programs (which make `su` and `passwd`
  possible), `umask`.
- Salted, deliberately slow password hashing.
- Owners for shared memory segments and for files on `/disk` (FAT has no
  place for them: either a small overlay file or a filesystem that has, see
  C2).
- **Depends on:** nothing.
- **Check:** `make test-login` extended: a user changes their own password
  through a set-uid `passwd`; an unprivileged user cannot.

### B6. A cryptographic random number generator (S) — done

Done (`src/drivers/char/random.c`): ChaCha20 with the key replaced after
every request, reseeded through SHA-256 from RDSEED or RDRAND and from the
timing of keyboard, disk and network interrupts. Known-answer tests for
ChaCha20 (RFC 8439, which the library's own test had never checked) and for
the generator step, both agreeing with an independent implementation; a
small statistical test in the kernel rather than a battery on the host.
On a CPU without a hardware generator (QEMU's default model) the seed at
boot is only as unpredictable as the timestamp counter.

- ASLR, ping ids and `/dev/random` use a xorshift generator. Replace it with
  ChaCha20 (already in `lib/crypto`) keyed from `RDSEED`/`RDRAND` where
  present and from interrupt timing otherwise.
- **Depends on:** nothing.
- **Check:** known-answer tests for the generator; `/dev/random` output
  passes a basic statistical battery run on the host.

---

## Track C — Storage and devices

### C1. FAT32: long names and lazy directories (M) — done

Done: long names read and written, rename and move (a new VFS operation and
system call, with `mv`), timestamps from the RTC, directories read on first
use. Checked by unit tests that re-read each directory from the disk, by the
image builder writing long names the kernel must read, and by `fsck.fat` on
an image the kernel has created, renamed, moved and removed in.

Different from the check below: the host has `mkfs.fat` and `fsck.fat` but
no tool to copy files into an image without mounting it, so the long names
"populated on the host" come from this repository's own `tools/mkfat.py`;
`fsck.fat` is the independent judge of both writers. Names outside ASCII
are not decoded (the short alias is shown), and lookup is case-sensitive.

- Long file name entries (read and write), so names are not limited to 8.3
  in one case; rename; real timestamps from the RTC.
- Read directories when they are first opened instead of walking the whole
  tree at boot.
- **Depends on:** nothing.
- **Check:** images written by `mkfs.fat` and populated on the host with
  long names read correctly; `fsck.fat` accepts what the kernel writes.

### C2. A block cache and a filesystem with owners (L) — done, except as the root

Done: a cache of disk sectors between the filesystems and the disk
(`src/drivers/block/bcache.c`), up to eight requests with a device at once,
more than one disk, and an ext2 driver (`src/fs/ext2.c`) mounted at `/home`
from the second disk, with owners, groups, modes and times stored on disk.
Reading a 100 KiB file took 442 disk requests; it now takes 203 the first
time and none the second. `make test-home` has a user create, change and
remove files, runs `e2fsck` on the result and boots again to find them.

Different from the proposal: the cache is write-through, not write-back
(delaying writes means keeping their order by other means, which belongs
with crash consistency, C3). And ext2 is mounted beside the root, not as
it: `/` is still the in-memory tree filled from the initrd, so `/etc`
(passwords included) does not persist. Making it the root means building
the system image onto that disk and booting from it, which is its own
piece of work.

- A buffer cache between filesystems and the disk driver (today every FAT
  access is a disk request), with write-back.
- Several requests in flight on the virtqueue instead of one.
- A small native filesystem (ext2 is the usual teaching choice) that can
  store owners, modes and timestamps, mounted as the root so that `/` itself
  is persistent.
- **Depends on:** C1 is independent; B5 benefits.
- **Check:** `make test-disk` against the new filesystem, checked on the host
  by `e2fsck`; a measured speed-up from the cache.

### C3. Crash consistency (M) — done for ext2, by ordered writes and a check at mount

Done: the first of the two options below. The ext2 driver orders its writes
so that an interruption can only leave blocks or inodes allocated and
unreferenced, a link count too high, or an inode whose size and block count
lag behind blocks already listed in a table of block numbers. `ext2_check`
runs at every mount (the machine has no orderly shutdown), walks the tree
from the root and puts those right; damage of any other kind it reports and
leaves alone. `make test-crash` kills QEMU at random moments while
`/bin/fsstress` rewrites the volume, boots again and runs `e2fsck -fn`:
thirty power cuts so far, on one and four CPUs, clean after each. Its third
round found the size-and-block-count case, which the first version of the
check did not repair.

Not done: a journal. Data in flight when the power goes is lost or partly
written; only the structure is guaranteed. The FAT volume has no such check:
after a crash it may have lost clusters, which `fsck.fat` reclaims.

- Either ordered writes with a consistency check at mount, or a simple
  journal for metadata.
- **Depends on:** C2.
- **Check:** a test target that kills QEMU at random points during a write
  workload and requires the volume to mount cleanly every time.

### C4. Devices worth having (S to M each, independent)

- **A one-shot timer** — done (`src/kernel/time/timer.c`). The periodic tick
  is stopped once the clock is calibrated; each CPU's local APIC timer fires
  once, for the earliest of: the running task's 10 ms, the deadline of a
  task that went to sleep on that CPU, output waiting to be shown. An idle
  CPU's timer is off. Two threads that used to wake "in case" had to learn
  to sleep until there was work: the reaper (PID 0) and `tcpd`. Without a
  timestamp counter the kernel stays periodic, and `make check` runs that
  too. Not done: device interrupts still all arrive on the boot CPU, and
  the tick count is still the unit most of the kernel thinks in.
- **virtio modern transport** — done (`src/drivers/virtio/virtio_pci.c`).
  Both drivers go through one layer that uses the modern interface
  (registers in memory, located by the PCI capability list) when a device
  offers it and the legacy I/O ports otherwise. Device registers are mapped
  uncached in a region of their own (`vmm_map_device`). Tested with devices
  offering both, legacy only and modern only (`VIRTIO=legacy|modern`).
  Not done: MSI-X (interrupts still come over the shared PCI line), and
  the packed virtqueue layout.
- **PS/2 mouse and a framebuffer device** — done: `/dev/mouse`
  (`src/drivers/input/mouse.c`), `/dev/fb0`, and `/bin/paint`, which uses
  both. A program that opens the screen for writing owns it until it closes
  the device or dies; file operations gained `opened`/`closed` hooks for
  that. Tested by moving the mouse through QEMU's monitor and checking
  screenshots (`make test-paint`). Not done: `mmap` of the screen (drawing
  is by `write`), `poll`, a window system; this is where one would start,
  as a user-space program, if one is wanted at all.
- **Swap** — done as designed below (`src/mm/swap.c`; `make test-swap`).
  What the first runs added to the design: the page a process is faulting
  on can be the one it has just pushed out, so the fault handler reads the
  faulting page back whatever kind of fault it was; and a process with
  nothing it may give up (a fresh fork child: every page still shared)
  must not search its whole address space at every fault. Known limits:
  a fork child cannot free memory until it has pages of its own, and a
  system call that needs a swapped page when no frame is free fails rather
  than making room. (Testing this showed that a dead process kept its
  memory until its parent collected it; it now gives it back as it dies.)
  The design, decided before the code:
  - *Where:* a disk of its own carrying the Linux swap signature (made on
    the host by `mkswap`, so the format is checked against someone else's
    implementation). One page per slot, read and written past the sector
    cache.
  - *What a swapped page looks like:* a page-table entry that is not
    present, marked as swapped, holding the slot number where the frame
    address was and the page's permission bits unchanged. Slots are
    counted: `fork` copies the entry and adds one, so parent and child each
    read their own copy back (copy-on-write without a shared frame).
  - *Whose pages:* a process's own, and only when it page-faults from user
    mode ("local replacement"). The kernel checks a program's buffers and
    then uses them, sometimes after sleeping and sometimes under a lock;
    a page taken in between would fault where nothing can sleep. A process
    faulting in ring 3 has no such buffer outstanding, and its address
    space is live on no other CPU, so its own pages can be taken with one
    `invlpg` and no cross-CPU work. Pages shared with another process
    (shared memory, copy-on-write not yet resolved) stay.
  - *Which page:* the clock algorithm over the process's own page tables,
    using the accessed bit as the second chance.
  - *When:* at a user-mode page fault, if free frames are below a low mark,
    until they are above it again. System calls only swap in.
  - *Promises:* `mmap` and `brk` may promise free frames plus free slots.
  - *Not in the design:* taking pages from other processes (global
    replacement), keeping the disk copy of a page that was read back and
    not changed, swapping to a file, more than one swap device.
  - **Check:** unit tests on an address space built for the purpose (out,
    back, fork, unmap, destroy, every slot returned); a program that uses
    more memory than a 64 MB machine has, with and without the swap disk.
- **Check:** each with its own unit tests and a scripted QEMU scenario.

---

## Track D — A system that lives on its disk (done)

Today `/` is a tree in memory, rebuilt from the boot image (the initrd) at
every start: a new user, a changed password or a file outside `/home` and
`/disk` is gone at the next boot. The machine starts into the kernel's own
debugging shell, and it has no way to shut down but to be switched off.
After this track it behaves like a Unix machine: the system is installed on
an ext2 disk, `/` is that disk, the kernel starts `/bin/init`, which puts a
login prompt on the console, and `shutdown` stops everything and leaves
the disk clean.

### D1. Mount points in the VFS (S) — done

An ext2 root has `/dev`, `/proc` and `/tmp` as ordinary empty directories,
and the kernel's own trees must sit on top of them, as `mount` does
elsewhere. The VFS keeps the whole tree in memory with nodes from several
filesystems, so a mount point is a node the kernel attaches in place of the
disk's directory of that name: the disk's entry is skipped when its parent
is read, and a mount point can be neither removed nor renamed (`EBUSY`).
- **Check:** unit tests mount a memory directory over an ext2 directory,
  read the parent from the disk again, and try to remove and rename it.

### D2. ext2 as the root (M) — done

- `tools/mkroot.sh` builds `build/root.img`: the user programs in `/bin`,
  `/etc` (passwords, groups, a profile), the home directories with their
  owners, the set-uid programs, empty `/dev`, `/proc`, `/tmp` and `/disk`.
  It replaces `home.img`: `/home` becomes a directory of the root.
- At boot the first disk holding an ext2 volume with `/bin/init` becomes
  `/`; the memory trees are mounted on it (D1). Without one, the kernel
  falls back to today's memory root from the initrd, which stays as the
  rescue system and is tested as such.
- **Check:** unit tests run with the disk root; a change to `/etc/passwd`
  made through `passwd` survives a reboot; `e2fsck` agrees after each run.

### D3. Booting into init and a login prompt (M) — done

- The kernel reads its command line from Limine. `limine.conf` gets two
  entries: the system (the default), and a developer entry whose command
  line asks for the kernel shell, as now. The scripted tests boot the
  developer entry; a new target boots the system.
- Without the developer flag, PID 1 is `/bin/init`. It runs `/etc/rc` with
  the shell, then keeps a `login` running on the console, starting a new one
  whenever the last ends, the way `getty` does. Orphans are still adopted
  and collected.
- **Check:** `make test-boot`: boot the system entry, log in at the prompt,
  run commands in the shell, log out, log in again as another user.

### D4. Shutdown and reboot (M) — done

- A system call, root only, that ends every process (a terminate signal,
  then a kill after a grace period), writes everything out, marks each
  ext2 volume clean, and powers the machine off through ACPI (S5: the
  `PM1a` control register from the FADT, the sleep type from the `\_S5`
  object in the DSDT) or resets it (the FADT reset register, or the
  keyboard controller).
- Mounting an ext2 volume clears its clean mark; the full check at mount
  (C3) runs only when the mark is missing, that is after a crash.
- `/bin/shutdown` (`-r` to reboot).
- **Check:** `make test-boot` ends with `shutdown`, and QEMU must exit by
  itself; the next boot finds the volume clean and skips the check; the
  crash test (C3) still finds it unclean and repairs it.

## Track E — Every function explained; nothing nobody uses

From 2026-10-18 the rule (CLAUDE.md) is that every function has a comment
saying what it does: its arguments and result, what it changes, and what a
caller has to know (locks held or taken, whether it may sleep). A function
other files call is documented once, at its declaration in the header; a
static one at its definition. New code follows the rule from now on; this
track brings the existing code up to it, and removes what nothing uses on
the way.

Measured on 2026-10-18 with `tools/undocumented.py --count` (a heuristic,
see the tool): 569 of about 1200 functions have no comment saying what they
do. Most are in user programs (108), the unused crypto library (83), the
tests (82), `src/fs` (61), the system calls (36) and the user C library
(30). `tools/unused.sh` (the linker's list of what nothing refers to)
finds about 50 files with uncalled functions: most of `lib/crypto` (AES and
its modes, BLAKE2s, Ed25519, MD5, SHA-1, SHA-512, Poly1305, SipHash, HKDF,
CRC32), and in the kernel proper a few dozen leftovers such as
`file_open_at`, `vfs_stat`, `signal_send`, `idt_set_gate`,
`ioapic_route_irq` and `klog_print_dmesg`. Some of those are only called by
tests (`tools/unused.sh tests` tells them apart), some only inlined.

One pass per area, each on its own branch and checked with `make check`:

1. `src/mm`, `src/kernel` (scheduler, processes, signals, locks, time) —
   done: every function documented; removed `user_set_brk` and the
   `user_mmap_anon` wrapper (left over from before B4, kept alive only by
   their own test), `signal_send` and `klog_init` (no caller, no effect),
   and three copies of `page_align_up` and of `rdtsc` (one each now, in
   `pmm.h` and `cpu.h`). The linker's list also exposed three kernel shell
   commands that `help` offered but that had been deleted by mistake
   (restored, and the shell made table-driven).
2. `src/arch/x86_64` — done: every function documented. Found on the way:
   the local and I/O APICs were reached at fixed addresses through the
   direct map (cached memory, and an address the firmware may put
   elsewhere); now from `IA32_APIC_BASE` and the MADT, through
   `vmm_map_device`. `acpi_init` trusted the root table's length (a short
   one underflowed into a huge entry count). A CPU that could not be given
   a stack still counted in the CPU total. Every ring-3 exception other
   than #GP and #PF reported `SIGKILL`; a division by zero now ends the
   program with `SIGFPE` and `ud2` with `SIGILL` (tested, with `sgdt` under
   UMIP). Removed: `ioapic_route_irq`, the public `idt_set_gate`,
   `smp_get_per_cpu` (a second name for `cpu_get`), six private
   declarations of `register_irq_handler` (now in `interrupts.h`).
3. `src/fs` — done: every function documented, and the comments in
   `vfs.h` put back next to the functions they describe. Found on the way:
   a name of 128 bytes or more was cut short in memory while the disk kept
   it whole, and `touch x/` made a file called "x/" (one check of new names
   in `vfs_create` now; `vfs_find_parent_at` returns the last name without
   its slashes, and an error saying why); the kernel's paths were limited
   to 127 bytes, shorter than `/tmp/` plus the longest name, and a longer
   one was reported as a bad address; `file_close` dropped the shared
   reference count without an atomic operation and the node's open count
   without the tree lock (two CPUs closing and opening could lose one, and
   a removed node be freed while open); the initrd parser could step past
   the end of an archive whose last entry is not padded; `/proc/cpuinfo`
   printed "1 running tasks" whatever the number; an ext2 rename that
   failed to read its inodes wrote one back from uninitialised memory; a
   failed ext2 mount left its mount point behind. Removed: `vfs_stat`,
   `file_open_at`, the FAT driver's copy of every write (system calls pass
   it kernel memory), and the two copies of the "forget a directory" test
   helper (one, `vfs_forget_directory`).
4. `src/drivers`, `src/net` — done: every function documented. Found on
   the way: `keyboard_init` waited for the PS/2 controller without limit,
   and a machine without one (most current PCs) hung at boot (bounded now;
   `make test-kshell` boots QEMU with `i8042=off`); the CMOS clock's index
   and data ports were used from any CPU without a lock, a read could
   straddle the clock's own update, and 12-hour mode turned 12 PM into 0
   and left 12 AM at 12; every IPv4 packet taught the ARP cache its
   sender, so a burst from distant hosts (whose frames carry the gateway's
   address) could push the gateway's entry out, and the receive thread,
   which may not wait for ARP, could no longer answer them. Removed:
   `rtc_init` (it only printed), `serial_write`, `pci_find_device` and two
   PCI fields nothing filled in, `virtio_net_rx_pending`, `dev.h`'s
   re-export of `random.h`, a second copy of each UDP datagram on its way
   to the caller.
5. `lib` — done: the unused crypto deleted (AES and its modes, BLAKE2s,
   Ed25519, HKDF, HMAC, MD5, Poly1305, SHA-1, SHA-512, SipHash, CRC32: about
   2000 lines nothing called), with `ctype`, `strcpy`, `strcat`, the empty
   `crypto_subsystem_init` and two headers that only included `string.h`.
   What is left (SHA-256, ChaCha20, the string functions) is documented,
   and SHA-256 is tested against whole published hashes instead of the
   first few bytes of one.
6. `user/lib`, then `user` — done: every function documented (a
   program's `main` by the comment its file starts with, which gives its
   usage; the library's one-line declarations by a comment on the same
   line: `tools/undocumented.py` accepts both). Found on the way: changing
   a password rewrote `/etc/passwd` from the first 4 KiB the library had
   read, deleting every account after them (now refused with `EFBIG`); a
   uid field that is not a number read as 0, which is root; `edit` loaded
   only the beginning of a large file, said so, and then saved that over
   the whole file; `malloc` of a huge size wrapped to a zero-byte block;
   `fdreadline` and `read_password` with a size of 0 wrote before the
   buffer; `strchr` did not find the terminator as C's does; `chown`
   accepted ids that overflow. Tested in `syscheck` (a 5.5 KiB account
   file, a bad uid) and `make test-edit` (a 580 KiB file is not saved).
7. `tests` — done: every function documented; the suites are declared
   once, in `test.h`, each with a line saying what it covers. Removed:
   `test_example.c` (it checked that 2 + 2 is 4), two copies of a "file
   holds this text" helper and of a remove helper (one each, in
   `test_runner.c`). Found: the disk test's workers took their numbers
   with a plain `++` on a shared counter, so on several CPUs two could get
   the same sectors. And `tools/qemu_shell.py` sometimes killed a QEMU
   that was in the middle of exiting, failing `make test-shutdown` about
   half the time after the third pass changed its timing (it waits for
   QEMU once its output has ended now).

In each pass: document every function to the rule; delete functions
nothing calls, unless a test is their reason to exist and the feature is
real (then say so in the comment); delete commented-out code and comments
that describe what the code no longer does. At the end, `make check` runs
`tools/undocumented.py --check` and fails if any function has no comment
(done: the count is 0).

## Track F — What a user of the system still misses

Proposed on 2026-10-20, after track E. Each item documents the code it
touches as it goes (every function a comment, enforced by `make check`).

### F1. The time of day (S) — done

Done as proposed (`src/kernel/time/walltime.c`, `/bin/date`, system calls
311 and 312). On the way, the VFS's file times changed from FAT's packed
format to Unix seconds (FAT converts at its edge), and the calendar
arithmetic moved to `lib/time/date.c`, shared with the user library. The
proposal:

Programs can ask how long the machine has been up, not what time it is:
there is no `date`, and `ls -l` shows FAT's packed dates. Every file
timestamp is a fresh read of the CMOS clock (a dozen port accesses, each a
trip out of the virtual machine). Read the clock once at boot, keep the
time as "boot time + the nanosecond clock", and give programs
`clock_gettime(CLOCK_REALTIME)` and `settimeofday` (root). The filesystems
take their times from it. A `date` program prints it (and root sets it).
**Check:** the kernel's time agrees with the CMOS clock to the second at
boot and advances with the monotonic clock; setting it moves both file
times and `date`; non-root is refused.

### F2. Waiting on several descriptors: `poll` (M) — done

Done as proposed (`src/fs/poll.c`, system call 7, `nc` in one process).
Pollers share one wake-up channel and a generation counter rather than
per-file wait lists; the terminal in line mode feeds keys already typed
through the line discipline when polled, since a line only exists once
they have been. The proposal:

A program can only wait on one thing at a time. `nc` has to choose between
the terminal and the connection, `paint` between the mouse and the
keyboard. `poll` takes a list of descriptors and the events wanted
(readable, writable, hung up) and sleeps until one is ready or a timeout
passes. Each kind of file says whether it is ready (pipes, the terminal,
TCP and UDP sockets, the mouse, regular files always) and wakes a common
channel when that may have changed.
**Check:** unit tests per kind of file; from ring 3, a child that writes to
one of two pipes after a delay wakes a parent polling both, with the right
one marked; `nc` relays in both directions in a scripted run.

### F3. Links on ext2 (M) — done

Done as proposed, with symbolic links in the tree kept in memory too (`/tmp`;
hard links there are refused: a file in memory is its node). Each name of
a hard-linked file has a node of its own; ext2 brings a node up to date
from the inode whenever the file is opened, so an append through one name
starts where a write through the other ended. On the way: the check at
mount reached only files' and directories' blocks, so a long symbolic
link's block would have been freed by it. The proposal:

Hard links exist on disk (the driver counts them) but cannot be made, and
symbolic links are skipped when a directory is read. Add `link`,
`symlink` and `readlink`; path lookup follows symbolic links (absolute and
relative, at most 8 deep, `ELOOP` beyond) except in the last component
where the call says not to (`unlink`, `lstat`-like uses). `ln` and `ln -s`
programs.
**Check:** links made by the kernel are accepted by `e2fsck`, and ones
made by `mke2fs -d` on the host are followed by the kernel; a loop of
links gives `ELOOP`; removing one name of a hard-linked file leaves the
other.

### F4. A faster `kmalloc` (S) — done

Done differently from the proposal: not size classes on top of the slab
allocator, but boundary tags (each block knows its own size and its
predecessor's, so freeing merges both neighbours at once) and free lists
by power-of-two size class (only the requested class is searched: any
block in a higher one is big enough). 2000 allocations take the same time
with 3000 blocks in use as with none. The proposal:

`kmalloc` and `kfree` walk every block of the heap (repair 5). Size classes
for small blocks (the slab allocator is already there) and a free list
kept in address order for the rest.
**Check:** the heap tests and leak checks still pass; `bench` shows
allocation time no longer growing with the number of live blocks.

### F5. Stacks for the moments the current one cannot be trusted (S) — done

Done as proposed. On QEMU the monitor's `nmi` reaches only the boot CPU
(it arrives through each CPU's LINT1, which the firmware sets up there
alone). PID 0's new stack is 16 KiB, against the bootloader's 128: the
boot and every unit test fit, and if one ever does not, the guard page
says so. The proposal:

PID 0 runs on the boot stack, which has no guard page (repair 4), and an
NMI arriving between `pop rsp` and `sysretq` would run on the user's stack
(repair 1). Give PID 0 an ordinary guarded kernel stack and NMIs an IST
stack of their own, with a handler that reports and returns.
**Check:** a stack overflow in PID 0's context is reported as one (a test
with a deliberately deep recursion in a kernel task stands in); an NMI
sent from the QEMU monitor (`nmi`) is reported and the system carries on.

---

## Track G — The shell, as someone at the keyboard meets it

Proposed on 2026-10-20, after track F. User-space work, with the kernel
changes only where the shell cannot do without them.

### G1. Command lists: `;`, `&&`, `||` (S) — done

Done as proposed, with `/bin/true` and `/bin/false`; `make test-jobs` runs
each form. `&` still only ends a whole line. The proposal:

A line may hold several pipelines: `a ; b` runs both, `a && b` runs b only
if a succeeded, `a || b` only if it failed. Scripts in `/etc` need it, and
so does anyone who types more than one thing at a time.
**Check:** `sh -c` runs of each form with programs that succeed and fail
(`true`-like and `false`-like exits), and the exit status of the whole.

### G2. Line editing and history at the prompt (M) — done

Done as proposed (`user/lib/lineedit.c`), with one kernel addition:
`isatty` (system call 313), because a shell reading a pipe must neither
edit nor prompt. That showed it had also been taking the terminal for job
control when reading a pipe; "interactive" now means reading the
terminal. The proposal:

The terminal's line discipline offers backspace and Ctrl-U; the arrow keys
do nothing. When it reads from the terminal, the shell edits the line
itself in raw mode: left and right, Home and End, Delete, Ctrl-A, Ctrl-E,
Ctrl-K, Ctrl-U, Ctrl-W, and up and down through the lines entered before
(the last 64, kept in memory). Programs the shell starts get line mode
back, as now. The editing is a function of the keys alone, so it can be
tested without a terminal.
**Check:** unit tests of the editor (keys in, line and cursor out) from
ring 3; a scripted session that recalls and edits a previous command.

### G3. Neighbours that move (S) — done

Done as proposed, with a request counter in the network statistics. The
receive thread's path (use the old answer, ask anyway) has no test of its
own: it would need a neighbour that changes its address. The proposal:

The ARP cache never forgets (repair 6): a machine that changes its
hardware address, or a gateway replaced, stays unreachable. Entries older
than a minute are asked about again by senders that may wait, and used as
they are by the receive thread (which may not wait) until the answer comes.
**Check:** a unit test that ages an entry and sees a fresh ARP request go
out; the network tests unchanged.

---

## Track H — Names at the prompt, and a socket that holds a burst

Proposed on 2026-10-20, after track G.

### H1. Wildcards in the shell (S) — done

Done as proposed (`user/lib/glob.c`); a shell line now takes 64 words in
4 KiB. `"<"x` is still two words: the shell does not join quoted and
unquoted parts of a word. The proposal:

`ls *.c`, `rm /tmp/f?`: an unquoted word with `*` or `?` (and `[...]`) is
replaced by the names it matches in its directory, sorted; a word that
matches nothing stays as it is, as in POSIX sh. Patterns in the last part
of a path only (`/tmp/*.txt`, not `/*/x`). User-space: `readdir` is all it
needs. The matcher is a function of two strings, tested on its own.
**Check:** syscheck tests of the matcher; a scripted shell session with
files made for it, quoted patterns left alone.

### H2. Tab completion at the prompt (S) — done

Done as proposed; a Tab after a word already complete lists the names of
the current directory (as bash does). The proposal:

Tab completes the word before the cursor: the first word from the
programs in `$PATH` and the built-ins, the others from the names in the
directory the word names. One match: completed (with `/` after a
directory); several: completed as far as they agree, and a second Tab
lists them. Built on the line editor (G2), with the name lookup passed
in, so the editor stays a function of its input.
**Check:** editor tests with a fixed list of names; a scripted session.

### H3. Socket queues sized in bytes (S) — done

Done as proposed; each datagram is charged 16 bytes on top of its payload,
so empty ones cannot fill memory for free. The proposal:

A UDP socket holds eight datagrams (repair 6), whatever their size: a
burst of nine small ones loses one while 8 KiB would hold them all. The
queue becomes a byte budget (64 KiB per socket), each datagram allocated
at its own size, and what does not fit is dropped and counted as now.
**Check:** a unit test that sends a burst of a hundred small datagrams
over loopback and receives them all, and one that overflows the budget
and sees exactly the excess dropped; no leak afterwards.

---

## Smaller repairs (any time, each S)

Known limits in code that is otherwise real, carried over from the previous
plan:

1. (Done in F5: NMIs have an IST stack and decide `swapgs` from the GS
   base. Other exceptions in that window would still run on the user's
   stack, and none can occur there.)
2. A killed process can leak a kernel buffer if it dies inside a system call
   holding one. (The two examples once given no longer apply: FAT writes
   allocate nothing, and a `/proc` read does not sleep while it holds its
   buffer, so a kill cannot take effect there. Disk and lock waits are
   uninterruptible.)
3. With the periodic tick (the fallback for CPUs without a timestamp
   counter) a few ticks are lost under heavy console output, because drawing
   runs with interrupts off. One-shot mode reads the count off the clock.
4. (Done in F5: PID 0 moves to a guarded kernel stack at boot.)
5. (Done in F4: `kmalloc` and `kfree` no longer walk the heap.)
6. (Done in G3 and H3: ARP answers age, and a UDP socket holds 64 KiB
   rather than eight datagrams.)
7. The forged-frame case of `sigreturn` has no targeted test (the frame is
   sanitised, and the fuzzer makes random `sigreturn` calls).
8. The `sysretq` non-canonical-address check cannot be demonstrated under
   QEMU, which behaves like AMD CPUs there.
9. The GitHub Actions workflow failed every run until 2026-10-20: the
   runner's gcc defaults to C17 and the code had come to depend on C23
   (`bool` without `<stdbool.h>`). Both builds now pass `-std=gnu17`. The
   first run after that built everything and failed on the runner's older
   `ld`, which warns about assembly objects without a `.note.GNU-stack`
   section (the local `ld` 2.46 no longer does); every `.S` file now has
   one.
10. (Done: the unused crypto was deleted in track E.)

---

## Recommended order

```
A4, B6, smaller repairs          quick, independent, all reduce risk
        |
A1 -> A2 -> A3                   the structural work; do it before the tree grows much more
        |
B3, B1                           the features users of the system notice most
        |
C1 -> C2 -> C3                   persistence done properly
        |
B2, B4, B5, C4                   as interest dictates
        |
D1 -> D2 -> D3 -> D4             the system on its disk (done)
E (one area at a time)           done: every function documented
        |
F1, F2, F3, F4, F5               done
        |
G1, G2, G3                       done
        |
H1, H2, H3                       done
```

If only one thing is done next, it should be **Track A**: every subsystem
added while "interrupts off" is the only lock makes the eventual SMP audit
longer, and it is the one item that changes how all later code must be
written.

## What "done" means for any item here

- Implemented, not stubbed; no placeholder left in the tree.
- Unit tests in `tests/`, ring-3 checks where programs can see the feature,
  and a scenario target if it involves a device or more than one boot.
- `make check` passes on both CPU models.
- README status table, `Documentation.md` and this file updated in the same
  commit.
