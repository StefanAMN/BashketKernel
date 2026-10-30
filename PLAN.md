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
itself, at the price of a contended lock and no cache affinity (a run queue
per CPU came with Q2). The boot CPU
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
DHCP server being there; `dhcp` replaces them (and with `-d` keeps the
lease renewed, L5). Not done: a resolver cache, record types other than A. The DNS check uses a server in
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

### B4. Memory: lazy and file-backed mappings (M) — done (the page cache came with P2)

Done: `mmap` and `brk` record a region and give each page its frame on first
touch; the pages promised and not yet touched are counted, and a call that
would promise more than is free fails. Private file mappings read each page
when it is touched (ten disk requests instead of 442 for one byte of a
100 KiB file). The kernel gives a program's buffer its frames before using
it, so its own accesses never fault.

Sharing a file's pages between processes came with the page cache (P2):
programs are read page by page (O2) into frames every process running
them maps. Shared file mappings and write-back came with Q1.
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

### B5. Permissions: the rest (M) — done (supplementary groups: L4)

Done: a group per process and per file with the group's permission digit,
the execute bit (checked by `execve`), set-uid programs with real and
effective user ids, `umask`, `su` and `passwd`, salted PBKDF2 password
hashes (checked against Python's implementation: `tools/mkpasswd.py` makes
the lines `/bin/login` accepts), and owners for shared memory segments.

Not done: set-gid and sticky bits (done in M1; set-gid directories in
O4). (Supplementary groups came with L4,
search permission on directories with smaller repair 11.) Owners for files on `/disk` wait for a filesystem
that can store them (C2), and a password changed with `passwd` is lost at
reboot for the same reason: `/etc` is in the initrd. (The password part no
longer holds since D2: `/etc`
is on the ext2 root, and `make test-shutdown` checks that a password
changed in one boot works in the next.)

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
  too. Not done: the tick count is still the unit most of the kernel
  thinks in. (Device interrupts reach other CPUs since N3, by MSI-X.)
- **virtio modern transport** — done (`src/drivers/virtio/virtio_pci.c`).
  Both drivers go through one layer that uses the modern interface
  (registers in memory, located by the PCI capability list) when a device
  offers it and the legacy I/O ports otherwise. Device registers are mapped
  uncached in a region of their own (`vmm_map_device`). Tested with devices
  offering both, legacy only and modern only (`VIRTIO=legacy|modern`).
  MSI-X came with N3. Not done: the packed virtqueue layout.
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
  a fork child cannot free memory until it has pages of its own; and
  nothing is fair between two processes swapping against each other
  (each takes room only from itself, and whichever faults at the right
  moment gets the frames): two 45 MiB programs once finished 20 seconds
  apart after a change that only moved code in the C library. (Found by
  `test-swap`, which had waited a fixed 8 seconds for the second.) (A
  system call that needed a swapped page when no frame was free used to
  fail rather than make room; M3 lifted that. Testing this showed that a dead process kept its
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

## Track I — Asking about a file, and the tools that need to

Proposed on 2026-10-20, after track H. A program can learn a file's size,
mode or owner today only by reading its directory; `cp`, `head`, `tail`,
`grep` and `sort` do not exist.

### I1. `stat`, `lstat` and `fstat` (S) — done

Done as proposed (system calls 314 to 316; a filesystem's `stat` op
supplies the link count and inode number). It needed the search
permission repair (11) first, which this item turned up. The proposal:

One structure (type, permission bits, owner, group, size, link count,
modification time, and the filesystem's inode or cluster number where it
has one) filled from the node: by path, following a final symbolic link
or not, and by descriptor (a pipe, a socket and the terminal report their
type and no size). The permission rule is the one `open` applies to
directories on the way; no right to read the file itself is needed, as in
POSIX. `ls` uses `lstat` instead of what `readdir` happens to carry.
**Check:** unit tests on the in-memory tree, FAT and ext2 (a hard link's
count, a symbolic link with and without following); syscheck from ring 3,
including a path through a directory the caller may not search (EACCES)
and a bad pointer (EFAULT); `/bin/stat`.

### I2. `cp`, `head`, `tail`, `grep`, `sort` (S) — done

Done as proposed, with a line reader for the user library and `make
test-tools`. `cp` needed I1 to say which filesystem a file is on, so it
can refuse to copy a file onto itself. `sort` keeps equal lines in input
order under `-r` too (GNU's `-s`). The shell has no command substitution
(backquotes), which the test first assumed. The proposal:

User programs, each reading files or standard input: `cp SRC DST` (or into
a directory; the mode is copied, the owner is the caller's), `head -n N`,
`tail -n N` (a file is read once, the last N lines kept in a ring),
`grep [-v] [-c] TEXT` (fixed text, not regular expressions), `sort [-r]
[-n]` (in memory, refusing input larger than it can hold rather than
losing lines).
**Check:** a scripted session that runs each against files with known
answers, and `cp` of a large file compared with `cmp`-like reading in
syscheck; the ext2 image passes `e2fsck` afterwards.

---

## Track J — Catching use after free; words and files the shell can make

Proposed on 2026-10-20, after track I. J1 comes from the `readlink` race
of repair 12, which no test could show; J2 and J3 from what `make
test-tools` first assumed the shell could do; J4 from `cp` and the
editor having to recreate a file to shorten it.

### J1. Freed heap memory is poisoned and checked (S) — done

Done as proposed. A small kmalloc/kfree pair costs about 0.3 µs more (the
F4 benchmark: 792 to 1385 µs alone, 695 to 1007 µs with 3000 blocks in
use); it still does not depend on the blocks in use. `make check` found
no write after free anywhere. The proposal:

`kfree` fills what it frees with a fixed byte (beyond the free-list
links), and so does heap growth; `kmalloc` checks that what it hands out
still holds that byte. A difference is a write after free: the kernel
panics, naming the block and the first changed offset. A read after free
then sees an obvious pattern rather than plausible data. `kheap_check`
verifies every free block. The cost is a pass over each block freed and
allocated, which the heap benchmark (F4) will show.
**Check:** a unit test that writes into freed memory and sees it
reported (the panic made a report for the test); the benchmark before
and after; `make check` passes with it on, which is itself the test that
nothing writes after freeing.

### J2. Words with quoted parts, and single quotes (S) — done

Done as proposed, but simpler for wildcards: a word with any quoted part
is not expanded at all (POSIX would expand `"dir"/*`; doing that needs
the matcher to know which characters were quoted). The H1 test had
relied on `"<"x` being two words and was corrected. The proposal:

`"<"x` is one word in POSIX sh and two here; `'$HOME'` is not expanded
there and is here (no single quotes). The word reader joins quoted and
unquoted parts until a blank or an operator, and learns single quotes
(no expansion at all inside). Wildcards stay text in either kind of
quote.
**Check:** a scripted session: joined words, both quotes, a quote inside
the other kind, an unclosed quote refused.

### J3. Command substitution, `$(...)` (S) — done

Done as proposed, with the child running the command line itself (a
fork of the shell, job control off) rather than starting `/bin/sh`. The
output is never read as shell text: `$(cat f)` cannot run what `f`
says. Unquoted output is split into words but not wildcard-expanded. The
proposal:

`echo "today: $(date)"`: the command runs in a child of the shell with
its output in a pipe, and the output (trailing newlines removed) takes
the place of `$(...)`, inside double quotes as one word, outside them
split at blanks. Nesting works because the child is a shell. Output
longer than a line (4 KiB) is refused rather than cut.
**Check:** a scripted session: in quotes and not, nested, a failing
command (its status in `$?`), too much output.

### J4. `truncate`, `ftruncate` and file times (M) — done

Done as proposed (system calls 317 to 319), and larger than the "S" it
was given: cutting an ext2 file part way through its block tree, in an
order that a crash can only leak from. The first tests grew files back
with `truncate`, which writes zeros over the cut block's tail itself, so
leaving the tail unzeroed went unnoticed; growing by one write at the far
end exposes it, on both filesystems. Devices and `/proc` files have no
length to change. The proposal:

Two system calls to set a file's length (shorter: the end is gone;
longer: zeros), on memory, FAT and ext2, permission as for writing;
and `utime` to set a file's modification time (the owner or root), so
that `touch` can exist. `/bin/touch` and `/bin/truncate`.
**Check:** unit tests on each filesystem (blocks freed on ext2 and FAT,
counted, `e2fsck` and `fsck.fat` happy); syscheck from ring 3, as root
and as a user without the right.

---

## Track K — Four at once

Proposed on 2026-10-20, after track J, to be built in parallel (each item
on its own branch, merged after one `make check` of all four).

### K1. Poisoned slab objects (S) — done

Done as proposed; fresh slab pages are poisoned too. The proposal:

J1 left the slab caches out: a freed `file_t` keeps its contents and is
handed out again unchecked. Freed objects are filled with a pattern
(beyond the cache's free-list link) and checked when allocated again, as
the heap does; a write after free panics, naming the cache and offset.
**Check:** a unit test that writes into a freed object and sees it
counted (as J1's test does); the unit tests otherwise unchanged.

### K2. `if`, `while` and `for` in the shell (M) — done

Done as proposed, with `elif` and `break N` / `continue N`; a text is now
parsed into a tree before it runs (`until` followed; `case`, functions
and `${NAME}` came with M2; redirections and pipes after `fi` / `done` / `}`
with N1; here-documents and `&` after a compound command with P3). Not
done: `for NAME in` split over lines. Ctrl-C stops a loop only by killing the command it is running.
The tests found `for 1 in a` accepted. The proposal:

`if LIST; then LIST; [else LIST;] fi`, `while LIST; do LIST; done` and
`for NAME in WORDS; do LIST; done`, on one line or over several lines of
a script (and at the prompt, which asks for more with "> "). Nesting.
`break`, `continue`. The condition is a command's exit status, so the
shell also gains `test` / `[` as a built-in for strings, numbers and
files (`-e`, `-f`, `-d`, `=`, `!=`, `-eq`, `-lt`...).
**Check:** syscheck runs `sh -c` with each form, nested, with
break/continue, false conditions, and syntax errors; a script file.

### K3. `find` and `du` (S) — done

Done as proposed. The first version kept every directory open on the way
down and ran out of descriptors 13 levels deep; both read a directory's
names and close it before descending. The "visited" check cannot be
exercised: links are not followed and directories have no hard links. The
proposal:

`find [DIR] [-name PATTERN] [-type f|d]`: every path under DIR (lstat,
not following links), filtered; `du [-s] [DIR]`: the space under each
directory in KiB. Both refuse to loop (a directory reached twice by dev
and inode is not entered again).
**Check:** `make test-tools` runs both against a tree made for it; a
loop made with a symbolic link is not followed.

### K4. `/proc/<pid>/` (M) — done

Done as proposed. A new directory op, `refresh`, brings `/proc` up to
date whenever anything looks inside it; nodes hold a pid, never a task
pointer, and every read looks the task up again, so a reaped process
reads as ENOENT. A dead process's nodes go at the next look into `/proc`
(leak counts must look once first). Memory in `status` is what the task
structure records, not a walk of page tables. Breaking the test found an
argument length left uninitialised at exec. The proposal:

A directory per process, appearing and disappearing with it: `status`
(name, state, user and group, parent, process group, memory in use) and
`cmdline` (its arguments, NUL-separated). `/proc/self` for the reader's
own. `ps` reads them instead of `/proc/tasks`.
**Check:** unit tests (a task's directory appears, describes it, and is
gone after it is reaped); syscheck reads its own `/proc/self/status` and
a child's `cmdline`; `ps` unchanged in what it prints.

---

## Track L — Small things a shell user reaches for

### L1. Redirecting any descriptor: `2> file`, `2>&1` (S) — done

`/bin/sh` could only point standard input and output somewhere, so error
messages could neither be kept nor sent down a pipe. A digit before `<`
or `>` now names the descriptor, and `N>&M` makes N a copy of M. A stage
keeps its redirections as a list in the order written, because the order
is the meaning: `a > f 2>&1` sends both to f, `a 2>&1 > f` only the
output. **Check:** `check_shell_redirects` in syscheck (both orders, the
pipe, `>>`, descriptors other than 0 to 2, a copy of a closed one, the
malformed forms). Its first sabotage passed: the test helper leaked a
descriptor into the shell, so `open` never handed out 3, the case under
test. The helper now starts the shell with 0, 1 and 2 only.

---

### L2. Shell variables, and a setting for one command (S) — done

`/bin/sh` knew only the environment (`export NAME=value`). Now
`NAME=value` on its own sets a variable of the shell, not passed to
programs (`$NAME` finds it, the environment second; a name is in one
place at most, so setting an exported one changes the environment);
`export NAME` moves one there, `unset` forgets either, and
`NAME=value command` sets it in that command's environment only (its
`PATH` too). The value is expanded like any word but not split or
matched: `X=$(ls)` keeps the newlines, `X=*` is a star. The status is
the last `$(...)`'s, as elsewhere (`X=$(false)` fails). A `for`
variable is now the shell's own rather than exported. Before a
built-in, the prefix form has no effect (none reads the environment).
`unsetenv` joined the C library. **Check:** syscheck
(`check_shell_variables`, 26 checks): the three forms, a plain variable
unseen by a child until exported, the prefix form not outliving its
command, values, statuses, `unset`, and names that are not names
(`1x=y` and `"X"=1` are commands). Sabotage: looking only in the
environment failed 13 checks (5 of them the existing `for` checks);
splitting the value as an ordinary word failed one, the `$(...)` value.

### L3. `uniq`, `cut` and `tr` (S) — done

Three more line tools on `user/lib/lines.c`, as POSIX describes them in
their basic forms: `uniq [-c] [-d]`, `cut -d C -f LIST` and `cut -c LIST`
(LIST: `N`, `N-M`, `N-`, `-M`, comma separated), `tr [-d] SET1 [SET2]`
with ranges (`a-z`). **Check:** `make test-tools` and syscheck, with
input lines longer than any fixed buffer and input without a final
newline.

Done as written. `tr` works on bytes, not lines, so it reads blocks and
needs no line reader; `cut -c` counts bytes (there are no multi-byte
characters here). `uniq` keeps a copy of the line it is counting, since
the reader reuses its buffer. Sabotage, one at a time: `uniq` comparing
only the first 2 KiB of a line failed the check with two 6000-byte lines
that differ in the middle; `cut -f` dropping a line without the
delimiter failed three checks; `tr` not repeating the last character of
a shorter SET2 failed one.

### L4. Supplementary groups (M) — done

A process is in one group; a user in several groups (`/etc/group`) can
only use one. Add a list of up to 16 more per process, copied by `fork`,
kept by `execve`; `setgroups` (root only) and `getgroups`; the
permission check counts a match with any of them. `/bin/login` and
`su` read `/etc/group` and set the list; `id` prints it. **Check:**
unit tests in `tests/` for the permission check; syscheck for the calls
(non-root refused, too many refused, fork copies); `make test-login`: a
user reads a file through a second group.
Done as described: `setgroups` is 320, `getgroups` 321; `/etc/group` is
`name:x:gid:members` (`user/lib/password.c` reads it); a file owner may
also `chown` a file into any group they are in. The test file is
`/srv/staff.txt` (0640, group staff, 50), made by `tools/mkroot.sh`.
Sabotage: with the list ignored by the check, the unit test, syscheck
and `test-login` failed; with `fork` not copying it, syscheck did.

### L5. DHCP lease renewal (S) — done

`dhcp` takes an address and exits; the lease runs out unnoticed. With
`-d` it stays, and renews at half the lease time (T1) by a unicast
REQUEST to the server that gave it, falling back to a broadcast at 7/8
(T2) and to starting over when the lease ends, as RFC 2131 section 4.4.5
says. T1 and T2 from the server (options 58 and 59) are used when they
make sense (0 < T1 < T2 < lease); a lease of 0 counts as a minute, so a
broken server cannot make us renew in a loop, and an infinite one is
never renewed. **Check:** QEMU's lease is fixed at a day, so the
arithmetic is a function checked on its own (`dhcp -T`, run by
syscheck), and `tools/test_net.py` renews at once (`dhcp -d -t 1 -n 2`:
T1 one second after each lease, exit after two renewals). It reads
QEMU's own capture of the traffic (`filter-dump`), not just what `dhcp`
prints: each renewal must be a REQUEST addressed to the server alone,
naming the address held, answered by an ACK. Not tested against a
server: rebinding and expiry, which only happen when the server stops
answering (QEMU always answers); the decision to enter them is in the
`-T` cases.

---

## Track M — Shared directories, a fuller shell, memory under pressure

### M1. The sticky bit and set-gid (S) — done

`/tmp` is writable by everyone, so anyone can delete or rename anyone
else's file in it. With the sticky bit (01000) on a directory, removing
or renaming an entry needs the caller to own the entry or the directory
(or be root). `/tmp` is made 1777. Set-gid (02000) on an executable runs
it with the file's group as the effective group, as set-uid does for the
user (the bit is dropped by the same writes and owner changes); set-gid
on directories (new files inherit the group) is left out and said so
(done later, in O4).
`ls -l` and `stat` show `t` and `s`; `chmod` accepts the four-digit
modes. **Check:** unit tests for the rule; syscheck as bob against
alice's file in `/tmp` (unlink, rename over it, rename it away: refused;
his own: allowed); `make test-login` runs a set-gid program.

Done as written. Until now every directory behaved as if sticky (removing
always needed ownership of the entry or the directory); the rule now
follows the bit, so a directory writable by all and not sticky lets anyone
remove from it, as in Unix. A process gained a real group beside its
effective one: `getgid` returns the real group, the new `getegid` (322)
the effective one, and `setgid` mirrors `setuid` (root sets both; anyone
else may only go back to the real group). `chmod` by a non-root owner
leaves the set-gid bit off a file whose group the owner is not in;
`chown` by root takes set-gid away when the group changes and set-uid
when the owner does. `/proc/<pid>/status` shows `Rgid`. `/srv/staff-id`
on the system disk is a set-gid copy of `id` for `make test-login`.
Sabotage, one at a time: ignoring the sticky bit in `auth_may_remove`
failed `permission_rules` and syscheck (eight checks, old and new);
ignoring set-gid at `execve` failed syscheck's set-gid program check and
`make test-login`.

### M2. `case`, functions and `${NAME}` in the shell (M) — done

Done as written, and `{ LIST; }` groups came with the function bodies.
`;;` is a segment of its own when a text is cut up; a case's patterns are
kept as written and expanded when it runs, a quoted `*`, `?` or `[`
going to the matcher in brackets (`[*]`), which needed no change to the
matcher beyond a variant without the hidden-name rule
(`glob_match_text`: `*` matches `.x` in a case). A function keeps a copy
of its tree, and each call runs a copy of that (a body may redefine the
function it is running). A call is run by the shell itself when it is a
command of its own, else by the child that a pipeline or a redirection
makes; `break` inside a function does not reach the caller's loops.
`"$@"` and `$@` both give one word per argument (an empty argument is
dropped, unlike POSIX). The depth limit (64 calls) abandons the whole
command text. Sabotage, one at a time: quoted pattern characters left as
wildcards failed two checks; a call not putting back its caller's
arguments failed one.

`case WORD in PATTERN|PATTERN) LIST;; ... esac` with the wildcard
matcher from `user/lib/glob.c`; functions (`name() { LIST; }`), called
like programs, with `$1`..`$9`, `$#`, `$@` inside, and `return [N]`;
`${NAME}` so that a name can be followed by letters. Functions live in
the shell that defined them (not passed to programs) and may call
themselves, up to a depth limit that fails cleanly. **Check:** syscheck
(`check_shell_case_functions`), including a recursive function stopped
by the limit, and `case` patterns with quotes (not wildcards).

### M3. System calls make room instead of failing (S) — done

Decided: record, do not reorder. `process_populate` (which every check
of a user buffer goes through) records the range in the task before it
populates it, joined to any range it touches, up to `TASK_CALL_RANGES`
(8); the system call entry clears the list as a call starts and as it
returns, and so does a page fault from user mode (the only other place a
range can be recorded is a signal frame set up on the way out of an
interrupt, which has been written by then). When a page needs a frame
and memory is low, or a frame could not be had, the process swaps out
its own pages through the usual clock, skipping every page of a recorded
range, and tries again (four times at most, each having freed at least
one page). "Make room before populating, from pages outside the range
about to be used" was not enough on its own: a call that checks two
buffers would lose the first while populating the second, and the kernel
would then fault on it with the access window open. A call that checks
more ranges than fit gives nothing up (it fails as before), because an
unrecorded range could be taken. There is one task per address space (no
threads), so no other CPU can be using a page that goes; the
`invlpg` on this CPU is the whole shootdown. A buffer larger than the
memory the process can free still fails with `EFAULT`: the kernel copies
from all of it at once.

Check: unit tests `kept_ranges_stay_in_memory` (no page overlapping a
range goes, partial pages included) and `call_ranges_join_and_fill_up`;
`swapper` ends with a 4 MiB `read` from `/dev/zero` into pages on the
disk while about 1 MiB is free, and `make test-swap` checks the line.
Sabotage: without the room-making in `process_populate` that read
returned `EFAULT`; with `vmm_swap_pick` ignoring the ranges, the unit
test failed ten checks while the swapper phase still passed (the clock
spares the buffer's pages, just read back and so marked accessed, and
takes others), so only the unit test guards the ranges.

The new phase also exposed a race in the fault path, 1 run of `make
test-swap` in 5 and once with `SMP=4`: with two swappers running, a
fault made room at its start, another process took the frames, and the
faulting process was killed ("out of memory for a promised page", and
once "could not be read back" in a fork child with no page of its own
to give). A user-mode fault that finds no frame now makes more room and
tries again, and when it has nothing to give, waits a tick for others
(a quarter of a second at most) before giving up (`fault_may_retry` in
`isr.c`, since O2 `swap_may_retry` in `swap.c`, which system calls use
too). Afterwards: 8 of 8, and 3 of 3 with `SMP=4`.

The plan as written:

A system call whose buffer is on the swap disk while no frame is free
fails with `EFAULT` (the known limit in C4): only a user-mode fault may
swap out, because a page taken while a call holds checked buffers could
be one of them. A call's buffers are all populated by
`vmm_user_range_ok` before any is used, so the call can make room
*before* it populates, from pages outside the ranges it is about to
use; or populate, and on failure swap out pages outside the current
range and retry. Decide which, and record why. **Check:** a unit test
on an address space built for it (no free frames, a buffer swapped out:
the call succeeds); `swapper` gains a phase that does `read` into
swapped pages while memory is full; `make test-swap` and
`make test-lowmem` keep passing.

### M4. `tee`, `xargs`, `seq`, `basename`, `dirname` (S) — done

Done as written, with two choices said in `user/xargs.c`: quotes and
backslashes in its input are ordinary characters, and empty input runs
nothing. `xargs` budgets each line against the kernel's own limits (32
arguments; 4096 bytes of arguments and environment together). Two
things the work turned up: a static array named `word` assembled, with
`-masm=intel`, as a reference to the size keyword `WORD`, so the code
wrote to its own text and faulted (no warning from the compiler or the
assembler); and the first check of the byte budget passed with the
environment left out of it, because the test's environment was small
enough to fit in the slack. It now runs `xargs` with a 1000-byte
variable, and the same sabotage fails it. Sabotaging `dirname` (keeping
the slashes between the parts) failed its check too.

The POSIX basic forms: `tee [-a] FILE...`, `xargs [-n N] [COMMAND
[ARG...]]` (arguments split at blanks and newlines, the command run as
often as needed so no argument list exceeds the limit),
`seq [FIRST [STEP]] LAST`, `basename PATH [SUFFIX]`, `dirname PATH`
(with the edge cases POSIX lists: `/`, `//`, trailing slashes, no slash).
**Check:** `make test-tools` and syscheck, including `xargs` with more
input than fits one command line.

---

## Track N — Interrupts of their own, and looking inside a process

### N1. Redirections and pipes after compound commands (S) — done

`for f in *; do cat $f; done > all` and `if ...; fi | wc` are refused:
a compound command cannot take a redirection or join a pipeline (K2's
"Not done"). A compound command followed by redirections or `|` runs in
a child of the shell (as a function call in a pipeline already does), so
its redirections apply to everything inside; one without stays in the
shell. **Check:** syscheck: each compound kind redirected, piped from and
into, `2>&1` after `done`, and that a variable set inside a piped loop
does not reach the shell (it ran in a child).

Done as written (`check_shell_compound_redirects`). A closing keyword
may now be followed by redirections or `|`, and a simple command's
`|` followed by `if`, `while`, `until`, `for`, `case` or `{` divides
it; a pipeline with a compound command in it, or a compound command
with redirections, is a node of its own whose commands each run in a
child, redirections applied after the pipe. A keyword now also ends at
an operator (`done>f`). Not taken: redirections on a function
definition (POSIX applies them at each call), and `&` after a compound
command. Sabotage: ignoring the redirections failed 13 checks, and at
first made syscheck hang instead: `cat` in a loop whose `< file` was
lost read the terminal. `sh_capture` now gives the shell `/dev/null` as
input, so a wrong read ends. Not splitting before `| while` failed the
three piped-into checks.

### N2. `/proc/<pid>/maps` and `/proc/<pid>/fd` (S) — done

Done, except shared memory in `maps`: `MAP_SHARED` and `shmat` map
frames straight into the page tables and the process keeps no record
of them, so `maps` shows the program (its segments, which the loader
now reports), the heap, the stack's 8 MiB and the `mmap` regions. Two
races had to be closed for reading another process's tables. A region
was unpinned before its entry was cleared, so a reader could find a
pointer to a file already removed; the entry now goes first, and the
reader holds the tree lock. A descriptor slot could be emptied and its
file freed while being described; slots are now emptied under a lock
(`fd_slot_set`), under which the reader takes its own reference
(`fd_slot_dup`). Sabotage: `fd_slot_dup` without the reference
crashed the kernel in `vfs_path_of` during syscheck (a freed file);
`maps` readable by everyone failed syscheck's check as bob. The first
version of the test checked that a closed descriptor left the listing,
and failed: opening the directory to list it takes the lowest free
descriptor, the one just closed.

What a process has mapped (start, end, permissions, kind: program,
heap, stack, anonymous, file with its name, shared memory) and what its
descriptors refer to (`/proc/<pid>/fd` lists numbers; reading
`/proc/<pid>/fd/N` gives a description such as `pipe`, `socket`,
`/tmp/x`). Readable by the process's owner and root only (they show
file names). **Check:** unit tests for the text; syscheck reads its own
maps after an `mmap` and a file mapping, and its fd list after `pipe`
and `open`; bob cannot read root's.

### N3. MSI-X for virtio devices (M) — done

Done for the modern interface. Each disk, and the network card's receive
queue, gets an MSI-X table entry and a CPU vector of its own (0x30 up,
`register_msi_handler`), sent to one CPU's local APIC; devices are spread
over the running CPUs, the boot CPU last since it takes every line. The
transmit queue and configuration changes get no vector (nothing waits
for them). The legacy interface keeps its line, on purpose: with MSI-X
enabled its register layout moves by four bytes (virtio 1.2, 4.1.4.8), a
second layout to maintain for no gain. If the device reads back
`VIRTIO_MSI_NO_VECTOR` for a queue, MSI-X is switched off again and the
line used, and the boot log says `interrupts: line IRQ n`, which every
test run on the modern interface refuses (as it refuses the wrong
interface); `test-smp` also requires a device sent to a CPU other than
the first. Handlers may run on a CPU that has not joined the scheduler
yet (disks are read during boot): that is safe, since spinlocks and
`sched_wakeup` need only `this_cpu()`. `/proc/interrupts` counts lines
and vectors per CPU. Sabotage: not writing the queue's vector made the
device fall back to the line, and `make test` refused the boot log;
messages that ignored the target APIC id failed the encoding test and
`disk_interrupts_arrive_by_their_route` (counted on the wrong CPU) under
`test-smp`. QEMU's `info irq` showed no I/O APIC interrupts on lines 10
and 11 while the disks were read. Not done: APIC ids above 255 (x2APIC
or interrupt remapping), more than 16 vectors, per-queue vectors on the
legacy interface.


Both virtio devices share one PCI interrupt line with each other (the
lesson in CLAUDE.md); with MSI-X each queue gets a vector of its own,
delivered to a chosen CPU's local APIC without the I/O APIC. Use it when
the modern interface offers the capability, keep the shared line
otherwise, and assert in the boot log which one is in use (the
lesson about fallbacks). Spread the vectors over CPUs. **Check:**
`make test-disk`, `make test-net` and `make test-smp` with both
`VIRTIO=modern` (MSI-X) and `VIRTIO=legacy` (the line); `/proc/interrupts`
(or `/proc/timers`'s neighbour) shows counts per vector and CPU; QEMU's
`info pci` / `info irq` confirm the line is no longer used.

### N4. `/etc/hosts` (S) — done

Names are only resolved by DNS. Look in `/etc/hosts` first (address,
then names, `#` comments), so `ping gateway` works with no DNS server.
**Check:** syscheck (a name in the file, an alias, a comment, a line too
long, a name that falls through to DNS), and `make test-net`.

Done as written (`hosts_lookup` in `user/lib/ulib.c`, used by
`resolve()`): the first line naming a name wins, names are compared
without regard to case as the DNS does, a line that does not start with
an address or is longer than 255 bytes is skipped whole. The file names
QEMU's user-mode addresses (`localhost`, `gateway`, `dns`); after `dhcp`
on another network they no longer apply, and nothing rewrites them.
Sabotage: `resolve()` skipping the file failed two syscheck checks and
`make test-net`. A check of `localhost` alone would not have noticed:
QEMU's DNS forwarder answers that name from the host.

---

## Track O — TCP that recovers, programs loaded on demand

### O1. TCP: reassembly and a measured retransmission time (M) — done

Done as written (`src/net/tcp.c`; the limits in `tcp.h`). Data kept out
of order needs no memory of its own: it is written into the free part
of the receive ring at the place its sequence number gives it, and a
list of at most eight stretches says what is there, so "memory
returned" is true by construction (and the leak checks still pass). The
receiver acknowledges each out-of-order segment on its own, since
coalesced acknowledgements would never add up to the three duplicates.
The RFC 6298 limits are changed for a virtual network (200 ms first
timeout and floor, 3 s cap), and a timer expiry still sends everything
from the oldest unacknowledged byte, since there is no congestion
window (P1 added one). In the loss test (one segment in seven dropped, 300 KiB):
before, 500 segments, 49 timer retransmissions, nothing kept; after,
475 segments, 28 timer and 15 fast retransmissions, 94 segments kept.
The first version of the hand-made reassembly test failed for a reason
of its own: it acknowledged the server's SYN+ACK before tcpd had sent
it, and an acknowledgement of something not yet sent is (rightly)
answered rather than believed. Sabotage: keeping nothing out of order
failed six checks (including the loss test's count); never fast
retransmitting failed the loss test's count.

A segment ahead of the expected one is dropped, and after a loss
everything from it on is sent again; the retransmission time is fixed
(`src/net/tcp.h`). Keep out-of-order segments (within the receive window,
bounded memory, overlapping and duplicate segments trimmed) and deliver
them when the gap fills; measure round trips and compute the timeout as
RFC 6298 says (SRTT, RTTVAR, Karn's rule, the 1 s floor relaxed to what a
virtual network needs, said so); fast retransmit on three duplicate
acknowledgements (RFC 5681 3.2, without the congestion window, which
stays out and is said so). **Check:** unit tests for the reassembly
queue (orders, overlaps, a segment beyond the window, memory returned)
and for the RTO arithmetic against RFC 6298's formulas; the existing
loss-injection tests with the counters showing fewer retransmissions;
`tools/test_net.py` against the host's TCP.

### O2. Programs loaded page by page (M) — done

`execve` copies every loadable segment in whole. Map file-backed
segments as private file mappings instead (B4's mechanism), so a page of
code is read when first run; `.bss` and the part of the last page past
the file's data are zero-filled. Relocations (programs are static PIE)
touch pages, which is fine: those become private copies. Without a page
cache, two processes running one program each read their own copy (said
so; P2 lifted that). **Check:** unit tests (a program whose untouched pages are never
read: disk request counter before and after); every scripted target, as
all programs now load this way; `make test-lowmem`, `make test-swap`,
`make fuzz`; the loader's existing hostile-ELF tests.

Done, for the read-only segments: `execve` lists the code and the
constants as program regions of the file (`elf_load_program`, `region.h`)
and reads them a page at a time at the first touch; the writable segment
is still copied, because the relocations patch it and a page shared with
`.bss` would need the file's bytes and zeros at once. The loader itself
now reads the file only where it needs it (headers, that segment, the
relocation table), not into a buffer of its size. A relocation aimed at a
segment left to the file is refused (its page is not there, and patching
it would be undone by the next read). Decided for a file that changes
under a running program: it cannot. Program regions hold the file open
(no removal: `EBUSY`) and count as running it (`exec_count`): opening it
for writing or truncating it is `ETXTBSY`, and a file open for writing
cannot be run, as in Unix. The untouched pages are promised, like mmap's,
so `/proc/meminfo`'s Promised is no longer 0 on an idle system (init's
code it has not run); `test-swap` and syscheck had assumed it was, and
syscheck now touches its own program before measuring. The check is
not a disk counter but the promise: a program asleep after starting
still has pages of its program untouched, never read
(`programs_are_read_page_by_page`).
Found on the way: with more faults per process, two processes swapping
against each other made a `read()` into swapped memory fail with `EFAULT`
3 runs in 6 (`test-swap`): `process_populate` gave up after four tries
to make room while the other process took it each time. It now waits as
a page fault does (`swap_may_retry`, at most a quarter second); 10 runs in
10 since. Sabotage: never leaving a segment to the file failed the two
new unit tests and syscheck's `ETXTBSY` checks; dropping the `ETXTBSY`
check on open failed four syscheck checks.

### O3. `$((...))` and `read` in the shell (S) — done

Arithmetic expansion with `+ - * / %`, comparisons, `&& || !`, parentheses
and variables, in 64-bit integers, division by zero an error (not a
crash); `read [-r] NAME...` assigning a line's fields (the last name takes
the rest), status 1 at end of input. **Check:** syscheck: precedence,
negative numbers, overflow wrapping as in C (said so), `$((x+1))` with
unset `x` as 0, errors; `read` from a pipe and a file, in a `while read`
loop.

Done as written (`check_shell_arith_read`), with C's precedence and
constants (`0x1f`, `010`); a variable must hold a number (blanks and a
sign allowed) or be empty; `&&` and `||` skip their right side, errors
in it included (`x != 0 && 10 / x`); overflow wraps, `LONG_MIN / -1`
too, instead of trapping. Not done: assignments inside (`=`, `+=`,
`++`), the bit operators, `?:`, and `$(...)` inside `$((...))`. `read`
takes a byte at a time, so the rest of the input is left for the next
reader; it runs in the shell itself (it sets the shell's variables),
its redirections applied there and undone after, and in a child when it
is part of a pipeline (`echo x | read v` sets nothing, as in other
Bourne shells). Found on the way: cutting a text into commands counted
the parentheses of `$(` but not quotes inside it, so `echo "$(echo a)";
echo b` ran as one command; a `$(...)` is now taken whole. Sabotage:
`&&` evaluating its right side failed two checks; `read` not giving
the last name the rest of the line failed five.

### O4. `mkdir -p`, `rmdir`, `rm -r`, and set-gid directories (S) — done

`mkdir -p` makes missing parents and accepts existing ones; `rmdir`
removes only empty directories; `rm -r` removes a tree without following
symbolic links; a directory with the set-gid bit (stored since M1, without
effect) gives new entries its group, and new subdirectories the bit.
**Check:** syscheck (a deep `-p`, `rm -r` over a tree with a link to a
directory outside it, which must survive; set-gid inheritance on memory
and ext2 directories) and `make test-home` (the group on disk after a
reboot).

Done as written. The kernel part is one function, `auth_new_entry_group`,
used where files, directories and links are made (`open` with `O_CREAT`,
`mkdir`, `symlink`); `mkdir` also copies the bit. `rm -r` reads a whole
directory before removing from it, refuses `/` and a last part `.` or
`..`, and enters at most 64 levels; `unlink` already removes a link by
its name, so `lstat` is the only care needed. `rmdir` refuses a link to
a directory. Sabotage, one at a time: the group not inherited failed the
rule on memory, as alice and on ext2 (three checks); `rm -r` with `stat`
instead of `lstat` followed the link and emptied the directory outside
(four checks); subdirectories not given the bit failed `test-home`'s
listing after the reboot.

---

## Track P — Sharing what is read, and a TCP that backs off

### P1. TCP congestion control (M) — done

O1 left the congestion window out: the sender is limited only by the
receiver's window. Add RFC 5681: slow start, congestion avoidance, the
reaction to a timeout (cwnd to one segment, ssthresh halved) and fast
recovery after fast retransmit (RFC 5681 3.2, or NewReno, RFC 6582, if
partial acknowledgements need it; say which). `/proc/net/tcp` or the
existing statistics show cwnd and ssthresh per connection. **Check:**
unit tests on the window arithmetic against the RFC's formulas; the
loss-injection test shows the window falling and growing again (a trace
of cwnd); `make test-net` against the host; no transfer slower than
before by more than a stated factor on the loss-free tests.

Done, with NewReno: with one segment in seven lost there are several
losses per window, and partial acknowledgements do occur (3 in the loss
test). The arithmetic is four functions with no state of their own
(`tcp_cc_*`, checked against the RFCs' equations worked by hand); each
connection's window is in `/proc/tcp` (not `/proc/net/tcp`: there is no
`/proc/net`). Two things the first runs added. A timer expiry while the
receiver's window is closed is a probe of that window, not a loss (RFC
9293 3.8.6.1), and must not shrink the congestion window: the loss-free
test's receiver sleeps before reading, and every transfer had dropped to
one segment at that point. And with windows of a few segments, three
duplicates rarely follow a loss, so most losses waited for the timer
(4 fast retransmits, 46 timeouts) until limited transmit (RFC 3042,
recommended by RFC 5681 3.2) let one new segment out on each of the
first two duplicates (15 fast, 32 timeouts). Times, 300 KiB over the
loopback: loss-free 309 ms before, 305 to 310 ms after; one segment in
seven lost (in both directions) 8.2 s before, 18.5 s after. That is the
point of congestion control at a 14% loss rate: the window stays at one
to five segments, so a lost acknowledgement or a lost retransmission
ends in a 200 ms timeout more often. Not done: selective
acknowledgement, early retransmit (RFC 5827), congestion window
validation (RFC 7661; the window is capped at `TCP_CWND_MAX` instead).
Sabotage: a sender that ignored the congestion window failed the new
hand-played test (it sent all 8000 bytes, not the four segments of the
initial window); counting a window probe as a loss failed the slow-start
check of the loss-free transfer.

### P2. A page cache for read-only file pages (L) — done

Two processes running one program each read and hold their own copy of
every page (O2's limit), and so do private file mappings. Keep file pages
in a cache keyed by (vnode, page index): a read-only mapping maps the
cached frame directly, counted; a private writable mapping copies on its
first write (as fork's COW). A file's cached pages are dropped when it
is written or truncated (or the writes go through the cache: choose and
say why), and the cache gives frames back under memory pressure (pages
mapped nowhere first). Swap does not take shared cache pages (as today
for shared COW pages). **Check:** unit tests (two mappings, one frame;
a write to the file seen by a new mapping; reclaim); a second `sh` costs
fewer frames than the first (`/proc/meminfo`); `make test-swap`,
`make test-lowmem` and `make fuzz` keep passing.

Done (`src/mm/pcache.c`), as decided before the code:
- *Coherence: dropping, not writing through.* Writes go to the file as
  before, and the file's cached pages are dropped after each write and
  truncation (and before its node is freed). Writing through would need
  every filesystem's write to go through the cache, for a gain only to
  mappings, which are private here anyway: a page already mapped keeps
  what it held, as before. A running program's file cannot be written
  (O2), so its pages never go stale. A generation number per file,
  raised by every drop, keeps a page that was being read while the file
  changed from going in afterwards.
- *Ownership: the frame sharing counts* (`cow.h`). The cache holds one
  reference and each mapping one more; a frame with one owner is mapped
  nowhere, and only such frames are ever freed by the cache. A frame
  still mapped when its file changes is let go by the cache and freed,
  as before, by its last mapping, so no frame is reused while a page
  table points at it and no shootdown of the cache's own is needed. A
  writable private mapping maps the cached frame copy-on-write, and the
  existing fault path makes its copy (taking the frame outright once the
  cache has let go of it).
- *Lock:* one spinlock, ranked 73 (above the heap and kernel stacks,
  below the page tables), held only to look up, insert and take out;
  under it only frame sharing counts are touched. Files are read, and
  frames and entries allocated and freed, outside it; a racing reader's
  page is used if it went in first.
- *Eviction:* pages mapped nowhere, least recently used first, when the
  cache passes 1/32 of memory (at least 128 pages), and on demand
  (`pcache_reclaim`) where frames are about to be needed: a user page
  fault with memory low, mmap and brk before they refuse, fork before
  its reserve check, and before swap pushes anything out. Swap does not
  take cached pages (they are shared with the cache). Kernel allocations
  do not reclaim (they happen under locks the cache ranks below); the
  cap bounds what they can find missing.
- *What is cached:* files in memory and files of a disk filesystem; not
  devices or `/proc` (nodes with operations and no truncate).

Measured: a second and a third `sh` cost 124 kB each, against 176 kB
with the cache switched off (13 pages of code shared); `/proc/meminfo`
gains a "Cached" line. Leak checks count pages mapped nowhere as free
(`test_free_frames`). Sabotage: never dropping on a write failed
syscheck's check that a new mapping sees the file's new contents, and
showed why the drop before a node is freed matters: a new file in `/tmp`,
given the freed node's address, read the old file's cached page, and
the kernel then crashed touching the freed node during reclaim. Mapping
a writable private mapping's page writable (no copy-on-write) failed
three syscheck checks (the other mappings saw the write; the existing
check, which reads the file back, did not notice). A lookup that never
hits failed ten unit checks.

### P3. Here-documents and `&` after compound commands (S) — done

Done as written (`check_shell_heredocs`, and a scenario in `test-jobs`).
The lines of a here-document are taken out of the command text when it
is cut into segments, the "<<WORD" replaced by a mark; the node made
from the command keeps the bodies, so a loop or a function expands them
again each time. A body reaches the command through a pipe: written at
once when the pipe holds it (4 KiB), by a process of its own otherwise
(the child of a child that exits, so init reaps it). A `-c` text or a
script command cannot carry more than 4 KiB of here-document (the
argument and pending-line limits), though expansion can make it longer.
A compound command ending in `&` runs as a job; outside the terminal it
is refused, as for a program. Sabotage: expansion turned off failed six
checks; `<<-` not taking tabs off failed one; dropping the `&` made the
`test-jobs` scenario fail. The first large-body check passed a 6000-byte
`-c` argument, which the kernel refuses (4 KiB of arguments): the text
is now made long by expanding a variable.

`cat <<EOF ... EOF` (with `$` expanded unless the word is quoted, and
`<<-` stripping leading tabs), in scripts and at the prompt (the prompt
asks for the lines with "> "); and `while ...; done &` as a background
job. **Check:** syscheck: expansion and quoting, `<<-`, two here-
documents on one line, one in a pipeline and a loop; `make test-jobs`
for a backgrounded loop stopped and continued.

### P4. Options for the text tools (S) — done

`grep -i -n -c -l -v` (combinations), `sort -n -r -u -k N -t C`,
`head -c N`, `tail -c N`, `wc -l -w -c`, `uniq -i`. POSIX's meanings, one
option parser in `user/lib` used by all of them. **Check:** syscheck and
`make test-tools`, including `sort -k` on fields of different widths and
`-n` with negative numbers and blanks.

Done as written (`user/lib/opts.c`, `check_tool_options`, and a "tool
options" scenario in `make test-tools`). The parser follows POSIX's
utility syntax guidelines: grouped letters, values joined or apart, `--`,
`-` an operand. `sort -n` compares numbers as text (sign, whole part with
leading zeros dropped, fraction), so numbers longer than a `long` order
correctly; `sort -k` without `-t` keeps the blanks before a field, as
POSIX says, so `-t` or `-n` is the way to sort columns of different
widths; `sort` now takes several files. `grep -l` wins over `-c` (as in
GNU grep). `wc` with one file still prints the counts alone (scripts
here read "N N N"); several files are named, with a total. Not done:
`sort -b`, `-f`, `-k` with character positions (`2.3`), and several
`-k` keys. Sabotage: `sort -n` ignoring the sign failed three checks; the
parser reading only the first letter of a group failed seven.

---

## Track Q — Writing through a mapping, a run queue per CPU

### Q1. Shared file mappings and `msync` (M) — done

`mmap(MAP_SHARED)` of a file is refused. With the page cache (P2) it can
map the cached frame writable in every process that shares it; changes
reach the file at `msync`, at `munmap`, when the last mapping goes and
at shutdown (dirty pages tracked by the page-table dirty bit, collected
from every mapping). Such a page stays in the cache while dirty (reclaim
writes it first, or skips it: choose and say why), is never swapped, and
`ftruncate` below a mapped page makes touching it `SIGBUS` (POSIX). The
file may be written with `write` meanwhile: say what coherence is kept
(P2 dropped pages on write; a shared mapping cannot simply be dropped).
**Check:** unit tests (two processes see each other's writes; `msync`
puts them in the file, checked by reading the disk after forgetting the
cache, CLAUDE.md's lesson); syscheck; `make test-home` (a change made
through a mapping is there after a reboot); `make test-crash` and
`make test-swap` keep passing.

Done (`src/mm/pcache.c`, `src/mm/region.c`, `msync` = system call 323,
`/bin/mapwrite`). The choices the item asked for:
- Dirty tracking is per cached page, not by the page-table dirty bit: a
  page mapped writable is dirty until written back with no mapping left
  that could write it again. Collecting dirty bits "from every mapping"
  needs every page table that maps a frame, and no reverse map is kept;
  walking other processes' tables is unsafe, as their exit frees them
  without the scheduler lock (Q4). The cost: a dirty page that is still
  mapped is written again by each `msync`, changed or not.
- Write-back happens at `msync` (both kinds at once: there is no writer
  thread), at `munmap` (after the pages are unmapped, so the last one out
  leaves the page clean), at exit and exec (after the address space is
  gone, for the same reason: `region_take_all`), and so at shutdown,
  which ends every process.
- Reclaim skips dirty pages rather than writing them (it runs under the
  cache's spinlock); a dirty page with no mapping is left only by a failed
  write.
- Coherence: a page a shared mapping has mapped is the file's one copy.
  `write()` changes it in place (other cached pages of the file are
  dropped, as before), `read()` copies it over what the file holds (it may
  have been written through a mapping and not saved yet), and a private
  mapping of the same page sees the changes until its first write gives
  it a copy (POSIX leaves that open).
- Truncation drops the shared pages past the new end and zeros the rest
  of the page the file now ends in. A page never touched is `SIGBUS` at
  the fault; one some process already maps is taken out of that process's
  page tables at its next safe point (system call return, interrupt
  return to user code), where it compares a counter of such truncations
  with the one it last saw (`region_revalidate`). Until then it can still
  read the old page: at most one scheduler tick for a process that is
  running.
- A writable shared mapping counts as open for writing: the file cannot
  be run meanwhile (`ETXTBSY`), as for `write()`.
Tests: `shared_pages_are_the_files_copy` and `writeback_reaches_the_disk`
(the second reads the file with the cache dropped); syscheck's
`check_shared_mappings` on a file in memory and one on the ext2 disk;
`make test-home` writes through a mapping with `mapwrite`, which exits
without `msync`, and reads it back after the reboot. Sabotage: `write()`
dropping shared pages failed the unit test and three syscheck checks; no
`SIGBUS` past the end failed three; no revalidation failed two; `read()`
not seeing mapped writes failed the unit test and one check (which first
passed by accident: a child's exit had already written the change back,
so a check of a write by the reader itself was added); write-back that
wrote nothing failed both unit tests and no syscheck check at all, since
`read()` shows the cached page, and with no write-back at exit
`make test-home` failed after the reboot. Not done: resident memory per
process (Q4 says why); a page a truncation extends the file over keeps
what a mapping wrote past the old end, where POSIX says zeros.

### Q2. A run queue per CPU (L) — done

One task list and one scheduler lock serve every CPU (A3's difference
from its plan). Give each CPU its own queue of runnable tasks and lock;
a woken task goes to the CPU it last ran on unless that CPU is busier
than the least-loaded one by more than a margin; an idle CPU takes work
from the busiest (stealing); tasks bound to a CPU stay. The scheduler
lock's held-across-switch rule (CLAUDE.md) and the zombie-stack rule
must survive: write down how. **Check:** `make test-smp` and every SMP
target; the existing four-tasks-on-four-CPUs measurement must not get
worse, and contention on the old lock (count spins before and after)
falls; a test that pins and unpins tasks; `make fuzz SMP=4`.

Design (written before the code). Two kinds of lock:
- The scheduler lock stays, for what it protects that is not "who runs
  next": the task list, sleeping and waking (state BLOCKED/STOPPED/ZOMBIE,
  wait channels, deadlines), signals, parents and reaping. Its callers
  outside sched.c do not change.
- Each CPU gets a run queue: its READY tasks in order, a count, and a
  lock ranked just above the scheduler lock (taken under it, never the
  other way round). The tick, yield and the idle loop take only their own
  queue's lock: that is the traffic that leaves the scheduler lock.
- Choosing: the head of this CPU's queue; if it is empty, the oldest task
  that may move (no affinity) from the queue of the CPU with the most
  waiting, its lock taken with a try-lock while holding ours (two CPUs
  taking from each other must not wait on each other: a refused try-lock
  means "not now"). A preempted task goes to the back of its own queue,
  or to its CPU's if its affinity changed meanwhile.
- Placing a woken or new task: the CPU it last ran on (its caches) unless
  that one has more than one task more than the least loaded (load: the
  queue plus the running task); a bound task goes to its CPU. An idle CPU
  that gets a task, or could take one, is sent the wake-up interrupt.
- Held across the switch: the outgoing task holds its CPU's queue lock
  through ctx_switch, and the incoming task releases it (in schedule
  after ctx_switch, or in sched_task_started), so a CPU taking a task
  from this queue cannot run it before it has left its stack. A task that
  sleeps, stops or exits holds the scheduler lock across the switch as
  well (a per-CPU note tells the incoming task to release it too): a
  waker or the reaper that sees BLOCKED, STOPPED or ZOMBIE under the
  scheduler lock therefore sees a task that is off its stack, which is
  the zombie-stack rule unchanged. schedule() returns with neither lock
  held; its callers take back what they need.

Done as designed (`src/kernel/sched/sched.c`; `spinlock_try_acquire`, and
a count of acquires that had to wait on every spinlock). Measured with
`yields_spare_the_scheduler_lock`, four tasks yielding as fast as they can
on four CPUs for 50 ticks: before, 1,077,461 yields and the scheduler lock
found held 667,023 times (62%; the same test, added to the old scheduler
for the measurement, fails there); now 1,565,959 yields and 0. The
four-workers measurement is no worse (one worker 57 ms, four 84 ms before;
66 and 89 now: within the host's noise; see below for the slow clock). Tests: that one, and
`pinned_tasks_stay_and_unpinned_ones_move` (with more busy tasks than
CPUs, a pinned task runs only on its CPU and waits only in its queue;
unpinned, it is soon seen elsewhere). Sabotage: placement and the
"may it stay here" check ignoring affinity failed the new test and every
older one that pins; no stealing failed the "seen elsewhere" check (with
every CPU busy, only stealing moves a task that is never woken). The
first `make check` failed that check with the periodic tick, three runs in
three: a CPU taking work chose the first of the busiest queues, so with
queues tied at one task each the highest numbered CPU (where the unpinned
task waited) was never chosen. Each CPU now looks from the one after itself.
That made it rarer, not gone (four in four runs on four host cores); the
test then printed the queues when it failed, which showed the real cause:
a CPU tried only the busiest queue, and CPU 0's often held nothing it may
take (the test runner, bound to CPU 0, waiting there at every tick), so it
gave up with movable tasks waiting elsewhere. Queues are now tried busiest
first until one gives a task. The same runs failed the four-worker check:
one worker took 50 ms (60 to 70 on the old scheduler, whose tick fought
for the lock) and four 130 ms on both, with a clock of 10 ms ticks. With
four times the work both schedulers measure 230 to 240 and 490 ms.
Not done:
moving work between busy CPUs (only an idle CPU takes work, and a woken
task is placed: a CPU with three tasks waiting and one with one stay so
until one goes idle), and timer queues per CPU for sleepers (the
scheduler lock still walks the task list for deadlines).

### Q3. `trap`, `set -e -u -x`, `shift`, `wait`, `local` in the shell (S) — done

`trap 'LIST' INT TERM EXIT` (and `trap - SIG`), `set -e` (stop at a
failing command, with POSIX's exceptions: conditions, `&&`/`||` lists,
`!`), `set -u` (an unset variable is an error), `set -x` (commands
printed to standard error with `+`), `shift [N]`, `wait [N]` for
background jobs, `local NAME[=value]` in functions. **Check:** syscheck,
one check per exception of `set -e`; `make test-jobs` for `wait` and a
`trap` on Ctrl-C.

Done (`user/sh.c`, syscheck's `check_shell_options`, a "trap and wait at
the terminal" scenario in `make test-jobs`). Added because the item needed
them: `! pipeline` (one of `set -e`'s exceptions) and `$$` (for a script
to signal itself in the trap checks). A signal handler only notes the
signal; the trap's list runs between commands (after the foreground
command has finished, as POSIX says), with `$?` and the place in the
running list (a pending `break`, Ctrl-C) kept around it. A child that
goes on running shell code is a subshell and drops the caught traps and
the EXIT trap; `trap '' SIG` is inherited by everything started. With an
INT trap, a command ended by Ctrl-C no longer ends the loop or list it
is in: the trap decides. `wait` catches Ctrl-C at the terminal (the
shell otherwise ignores it there) and returns 130, the job running on.
`set -u` in a shell that is not interactive exits with 2, running the
EXIT trap. `set` alone lists the variables. Not done: `set -- ARGS`
(setting `$1` ...), `set -o`, `$-`, traps for signals other than HUP,
INT, QUIT, USR1, USR2 and TERM. At the terminal, Ctrl-C at the prompt is
a key for the line editor, not a signal, so an INT trap of the
interactive shell itself runs only during `wait`. Sabotage: removing
the four exceptions failed their eight checks one each; breaking `-e`,
`-u`, `-x`, `local`, `$$`, the handler and the subshell reset failed 21
checks; a `shift` that did nothing first hung the `while ... shift`
check (it is bounded now) and then failed both; the terminal scenario
failed all three parts.

### Q4. `/bin/top` (S) — done

A full-screen list of processes, refreshed every second, sorted by CPU
use over the last interval (from the tick counts in `/proc/<pid>/status`,
which may need a field), with memory per process, the load of each CPU
and the totals of `/proc/meminfo`; `q` quits, the terminal restored
(raw mode dies with the program, but `top` should restore it itself).
**Check:** a scripted run (`tools/qemu_shell.py` with `/bin/spin`
running) that finds the spinning process at the top, then `q`; the
terminal usable after.

Done (`user/top.c`, `make test-top` on one CPU and on four). CPU use per
process is the difference in `Ticks` between two refreshes over the ticks
that passed; a process missing from the previous refresh is capped at the
whole interval. The load of each CPU needed a field: `cpu_t.busy_ticks`,
counted in `sched_tick` unless the idle task is running, shown as a line
per CPU at the end of `/proc/cpuinfo` (`proc_cpuinfo_counts_busy_ticks_per_cpu`;
sabotage: never counting failed it, and counting the idle task failed it
under the periodic tick). Memory per process is `Size`, a new line in
`/proc/<pid>/status`: the program's segments, heap and mappings, as
`maps` lists them, without the stack's 8 MiB reservation (syscheck: a
256 KiB mapping adds exactly 256 and goes again with `munmap`; leaving
mappings out failed it). Not resident memory: counting present pages
means walking another process's page tables, and exit and `execve`
destroy those without the scheduler lock, so a reader could follow a
freed table; doing it needs that teardown changed, or a count kept on
every map, unmap, copy-on-write, swap and fork path. Sabotage of the
scripted test: sorting the quietest first failed it. `top` leaving raw
mode on did not, and cannot: raw mode dies with its program (CLAUDE.md),
so the terminal is restored either way, and `top`'s own `tty_raw(0)` is
not visible from outside.

---

## Track R — What processes hold, who owns the terminal, CPUs that share

Planned 2026-10-25, after track Q. Each item comes from a limit that Q or
an earlier track wrote down.

### R1. Resident memory per process (M) — done

`/proc/<pid>/status` and `top` show a process's size, not what it holds
(Q4): counting present pages means walking another process's page tables,
which exit and `execve` free without any lock a reader could take. Give
each address space a lock (or a reference the reader holds) that its
teardown waits for, so `/proc` can walk it safely; report "Resident"
(present pages, and how many of them are shared) and show it in `top`.
**Check:** a unit test whose process touches N pages and is seen to hold
N more; a test reading the status of processes that are exiting and
exec'ing in a loop on four CPUs (no fault, no stale count); `make
test-top`; sabotage the teardown's wait and see the race test fail.

Done. No new lock: page tables are freed only when a whole address space
is destroyed (never by unmapping), so `/proc` walks a task's tables under
the scheduler lock it already holds, and `exit` and `execve` swap the
task's tables out under that lock before destroying them. A walk under
way finishes first; none starts on tables being freed. The walk visits
only the tables that exist (`vmm_count_user_pages`); "shared" reads each
frame's owner count without its lock (ranked below the scheduler's), a
snapshot. `/proc/<pid>/status` gains `Resident` and `Shared`, `top` a RES
column. Checks: syscheck (64 pages touched add exactly 256 kB, after a
warm-up round, since programs are read in page by page; `munmap` takes
them back; after `fork` `Shared` grows); `make test-top` wants a resident
`spin`; `status_of_exiting_and_execing_processes` reads the status of
shells and their exec'ing children as they come and go on four CPUs.
The sabotage the plan asked for (tables freed while still published)
passed that test twice, 8,600 reads over freed tables: a freed frame
keeps its contents and stays mapped in the direct map, so the walk found
what it expected. The test kernel now fills freed frames with 0xFF before
they can be handed out again (`pmm_free_frames`): a freed table then
points far past memory, and the same sabotage faulted in the walk, two
runs in two; everything else passes with it. The test also first failed
by itself: its reader read the file in 7-byte pieces, and each read makes
the text afresh, so a process changing between pieces gave a torn line;
it now reads in one call, as `cat` does.

### R2. Sessions and background jobs at the terminal (M) — done

There are no sessions, and a background job that reads the terminal is
not stopped (README). Add `setsid`, a controlling terminal per session,
and the terminal's checks: a process of a background group that reads it
gets `SIGTTIN` (and stops), one that writes gets `SIGTTOU` only with the
terminal's `tostop` set; the shell reports "Stopped (tty input)" and `fg`
gives the terminal back. **Check:** syscheck (`setsid` errors, a child in
its own session has no terminal); `make test-jobs` (`cat &` stops, `fg`
lets it read).

Done (`process_terminal_access`, called by the console's read and write in
the file layer, where the process holds nothing and may stop;
`setsid` 324, `getsid` 325, `tty_tostop` 326, `/bin/stty`, a `Sid` line in
`/proc/<pid>/status`, `SIGTTIN` 21 and `SIGTTOU` 22, which stop by
default). The kernel's processes share the console's session, numbered 0
and without a leader: making each a session leader would have refused
them `setpgid`, which POSIX denies a leader. A new session has no terminal
and cannot get one (there is one console). Found on the way: `init` is in
the console's session here, so a job it adopted was not orphaned by
POSIX's rule, and stopped for good; init now counts as outside every
session, as on a system where it leads its own. Checks: syscheck's
`check_sessions_and_terminal` (bounded: a child that neither stops nor
exits is killed after 5 s, so a broken check fails instead of hanging,
which the first sabotage did); `make test-jobs` (`cat &` stopped and
reported, then read after `fg`; `echo &` with `tostop`). Sabotage: no
terminal checks failed five syscheck checks and the scenario; no orphan
rule and a `setsid` that changed nothing failed four.

### R3. Work moves between busy CPUs; sleepers per CPU (M) — done

Only an idle CPU takes work (Q2): three tasks waiting on one CPU and one
on another stay so. Let a CPU whose queue is longer than the shortest by
more than one hand a task over at its tick; and keep each CPU's timed
sleepers in a queue of its own, ordered by deadline, so that the timer no
longer walks every task under the scheduler lock. **Check:**
`make test-smp` and the SMP targets; a test that starts six busy tasks
bound to one CPU, unbinds them, and sees them spread over four; the
yield measurement of Q2 stays at no waits; `/proc/timers` shows sleepers
per CPU.

Done. At a busy CPU's tick, `hand_over_work` moves its oldest movable task
to the least loaded CPU when its load is more than one above that one's
(its own queue lock held, the other's taken with a try-lock, as in steal).
Timed sleepers go into their CPU's list, sorted by deadline, under the
scheduler lock that sleeping and waking already hold; a one-shot timer
wakes from the front of its own list, the periodic tick from the front of
each, with every deadline kept on one scale. `/proc/timers` says how many
sleep with a deadline on each CPU. Tests: `busy_cpus_hand_work_over` (two
tasks bound to each other CPU keep it busy and never dry, so only handing
over can move the six waiting on CPU 0) and
`sleepers_wake_in_deadline_order` (eight sleepers started latest deadline
first). Sabotage: no handing over failed the first; appending sleepers out
of order failed the second and four older tests of sleeping and `poll`.
The second first failed by itself with the periodic tick: it looked for
the sleepers one tick after they had started, not once they slept.
Still open: the scheduler lock is taken for every wake-up, as before.

### R4. Interrupt lines spread over the CPUs (S) — done

Interrupts that come through the I/O APIC (keyboard, serial line, mouse,
legacy virtio) all go to the boot CPU; MSI-X vectors are already spread
(N3). Route each line to a CPU of its own, the same way, and say which in
`/proc/interrupts`. **Check:** `make test VIRTIO=legacy` and the SMP
targets with the legacy interface; a check that some line's interrupts
arrive on a CPU other than 0; the keyboard and serial line still work
(`make test-kshell`, the scripted shell tests).

Done (`ioapic_spread_lines`, `ioapic_route_isa_irq_to`; the round robin
the virtio drivers used is now `smp_next_interrupt_cpu`, for lines and
vectors alike). The serial line goes first, so on two or more CPUs it is
always CPU 1's: every program run of `make test-smp` reads
`/proc/interrupts` and expects none of its interrupts on CPU 0 (never
spreading the lines failed it); `make check` runs `test-smp` with the
legacy interface too. Seen there: the legacy disk's line can land on the
boot CPU when the turn comes round to it (it took 33,000 interrupts in one
run); the boot CPU is last in the turn, not left out.

---

## Track S — The screen as memory, larger windows and datagrams, shell arguments

Planned 2026-10-26, after track R, from limits the README and earlier
tracks wrote down (each checked in the code first).

### S1. The screen mapped into a program, and mouse and keyboard at once (M) — done

`/dev/fb0` can only be written with `write()` and `lseek()`, and nothing
has waited for the mouse and the keyboard together (both answer `poll`,
untested). Let the program that owns the screen map it (`mmap` of
`/dev/fb0`, shared, writable; the frames are the framebuffer's own, not
the page cache's, mapped uncached or write-combining as the device needs;
refused to anyone but the screen's owner, and gone when it gives the
screen back or dies), and make `/bin/paint` draw through the mapping and
`poll` both devices. **Check:** `make test-paint` (screenshots as now);
syscheck (mapping refused without owning the screen, refused past its
size; `poll` on console and mouse reports the one that has input); a
screenshot taken after the owner is killed shows the console again.

Done. A vnode operation `map_page` gives a device's physical page for an
offset (`/dev/fb0`: the framebuffer's address in the direct map, less the
map's offset). Its pages are mapped with a software bit, `VMM_DEVICE` (bit
52), with `VMM_SHARED`: unmapping and destroying skip giving them back,
`fork` copies them without counting owners, swap skips shared pages
already. A device region promises no memory, and holds the device through
its `opened`/`closed` operations, once per piece. Mapped as the kernel's
direct map has it (the same caching for both views). Checks: syscheck's
`check_screen_mapping` (read-only descriptor, past the end, private all
refused; a pixel written through the mapping is read back through the
file; with the descriptor closed, the mapping alone keeps the console off
the screen); `make test-paint` (draws through the mapping, polls both
devices, a third session ends with `q`; the killed session is now one
holding a mapping); the paint and SMP program runs refuse "PMM WARNING".
Sabotage: unmapping that gave the device's page to the frame allocator
printed that warning; a mapping that held nothing failed the holding
check; `map_page` ignoring the offset failed two checks and the
screenshots. (A mouse event and a typed line arriving together is only
exercised by `paint` itself.)

### S2. TCP window scaling (M) — done

A connection's window stops at 64 KiB (no window scale option, RFC 7323
section 2), which limits a transfer to one window per round trip. Offer
and accept the option, scale the window advertised and the one obeyed,
and let a receive buffer grow past 64 KiB. **Check:** unit tests of the
option (offered, accepted, refused when the other end does not offer it,
a shift above 14 clamped); `make test-net` against the host's TCP (which
scales), with the transfer time before and after written down; the
reassembly and congestion tests of O1 and P1 keep passing.

Done (`src/net/tcp.c`, `tcp_parse_options`). Every SYN we send offers a
shift of 3 (256 KiB >> 3 fits the field); a SYN+ACK carries the option only
if the SYN did, and scaling is used only when both carried it. Windows in
SYNs are never scaled; the one we advertise is rounded down so it never
offers room we lack; ssthresh starts at 65535 in the other end's units.
The rings start at 8 KiB and double up to 256 KiB (64 KiB unscaled): the
receive ring when the other end used all the window offered and the reader then emptied the ring, the send ring
when it is full while both windows allow more in flight. `/proc/tcp` shows
the shifts and ring sizes.

The plan was wrong about the host: the guest's TCP peer is QEMU's
user-mode network (slirp), not the host's kernel, and its SYN+ACK does not
carry the option (seen in a capture: our SYN `wscale 3`, its answer `mss
1460` only). So against the host every connection is unscaled, which at
least exercises "the other end does not offer it" against a TCP we did
not write. Times, 2 MiB echoed through `tcpecho` (`make test-net`, three
runs each): before 0.09, 0.10, 0.11 s; after 0.12 s each. Over loopback,
4 MiB with both rings grown to 256 KiB took 140 ms, and 149 ms with the
rings held at 8 KiB. Neither path has latency to speak of, so a larger
window buys almost nothing there. With a reader that takes what has
arrived once a tick (standing in for a round trip), 4 MiB took 5,127 ms
with the receive ring held at 8 KiB and 677 ms with it grown to 256 KiB.

The first trigger for the receive ring ("it filled up") passed on one CPU
and failed on four: there the reader empties the ring as data arrives, so
it never fills even while the sender is held to a window per round trip.
The trigger is now the one BSD uses: data reaching (within a segment) the
right edge of the window last offered. Over loopback the send ring is
never what holds a connection back (acknowledgements return at once), so
its test plays the receiver by hand, offering 8192 << 5 bytes and
acknowledging once a tick. Sabotage: rings that never grow (each trigger
removed in turn), the other end's windows or ours left unscaled, and
scaling assumed without the option each failed the new checks.

### S3. IPv4 fragments (M) — done

Fragments are dropped on arrival, and nothing larger than the link's MTU
can be sent ("don't fragment" is set). Reassemble arriving fragments
(RFC 791, holes as in RFC 815; a time limit and a memory limit, overlaps
refused), and fragment outgoing datagrams that do not fit. **Check:**
unit tests (out of order, duplicates, overlaps, a missing piece timing
out, the memory limit); `tools/test_net.py` exchanges an 8 KiB UDP
datagram with the host in both directions (the host's stack fragments and
reassembles: something we did not write).

Done (`src/net/ipfrag.c`; `ipv4_send_parts` in `net.c`). The holes of RFC
815 are the gaps between the pieces held, kept in order of offset; a
fragment must fall inside one. An exact repeat is ignored, any other
overlap gives up the datagram, as does a second end, data beyond the end,
an end past 65,515 bytes, or a non-final fragment that is not a multiple
of 8 bytes. At most 8 datagrams and 256 KiB are in progress (each piece
charged 32 bytes besides its data), and the oldest gives way. A datagram
not complete 30 s after its first fragment is given up by the receive
thread, which now waits for that deadline as well as for frames. UDP
datagrams go up to 65,507 bytes; `sendto` and `recvfrom` copy through a
buffer the task owns, since 64 KiB does not fit on a kernel stack. The
IPv4 identification is now taken atomically: two CPUs could give two
datagrams the same number, which reassembly would mix. Sabotage: no
overlap check failed five checks; the receive thread not told of a new
deadline failed the timeout check; no memory limit failed two; fragments
not a multiple of 8 failed the 64 KiB loopback test and syscheck; no
reassembled datagram delivered failed `make test-net` ("echoed 2 of 3").
Not done: the ICMP "reassembly time exceeded" message (RFC 1122, 3.3.2).

### S4. `set -- ARGS`, `$*` and `getopts` in the shell (S) — done

The shell cannot set its arguments ("set -- a b" is refused, Q3), has no
`$*`, and no `getopts`. Add `set -- ARGS` (and `set ARGS`), `$*` and
`"$*"` (one word, joined by a blank), and `getopts OPTSTRING NAME` with
`OPTIND` and `OPTARG`, as POSIX describes. **Check:** syscheck, a check
per case (`set --` with none, `"$@"` against `"$*"`, `getopts` with
grouped options, an argument joined and apart, an unknown option, `--`).

Done (`user/sh.c`; syscheck's `check_shell_arguments`). Values are never
split into words here (only `$(...)` output is), so unquoted `$*` is one
word per argument, like `$@`. What a function's `set --` makes is the
function's and is freed when it returns. Sabotage: a `set` that changed
nothing failed nine checks; `"$*"` not joined failed one; a `getopts` that
lost its place in "-ab" first hung the check (bounded since), then failed
it; a function's array never freed printed nothing different at all, and
was caught only by a check that the shell's size stays the same over
2,000 calls, which itself first passed because `cut -f2` (no space) gave
two empty strings that compared equal (it now wants a value).

---

## Track T — Errors that reach the sender, losses repaired together, arithmetic that assigns

Planned 2026-10-26, after track S, from limits written down earlier (each
checked in the code first).

### T1. ICMP errors, sent and taken in (M)

A UDP datagram for a port nobody has open is counted and dropped in
silence, and so is every ICMP message but echo: the sender of a datagram
to a closed port waits for an answer that will never come, and a TCP
connection the network reports unreachable runs out its retransmissions
(about 20 s) instead of failing at once. S3 also left out the "reassembly
time exceeded" message. Send "port unreachable" for UDP to a closed port
and "protocol unreachable" for an unknown protocol (RFC 1122 3.2.2.1, a
SHOULD), and "fragment reassembly time exceeded" when a datagram whose
first fragment arrived is given up (3.3.2); never in answer to an ICMP
error, a broadcast, or a fragment other than the first (RFC 1122
3.2.2), and no more than a few per second (rate limit, RFC 1812 4.3.2.8).
Take in "destination unreachable": codes 2 to 4 (protocol, port,
fragmentation needed) are hard errors and end the TCP connection they
quote (RFC 1122 4.2.3.9), the others are soft (remembered, reported if
the connection then times out); the quoted header must match a
connection's addresses, ports and a sequence number in flight (RFC 5927:
forged errors). **Check:** unit tests (a datagram to a closed port
answered, a broadcast one not, an error not answered, the rate limit,
an error quoting a connecting socket ends it with ECONNREFUSED, one
quoting a sequence number not in flight ignored); `tools/test_net.py`:
QEMU's capture shows a correct port-unreachable message for a datagram
from the host to a closed port, checked by a parser that is not ours.

### T2. TCP selective acknowledgement (M)

With NewReno a window with several losses is repaired one hole per round
trip (O2, P1 wrote it down). Offer and accept SACK (RFC 2018: the option
in the SYN, blocks of what arrived beyond a gap in each acknowledgement
while a gap exists, the most recent first), and as a sender keep a
scoreboard and retransmit the holes it shows in one round trip (RFC 6675
loss recovery). **Check:** unit tests of the blocks sent and of the
scoreboard; the lossy loopback transfer (`tcp_survives_lost_segments`)
with fewer retransmission timeouts and less time than with SACK refused,
both written down; a peer that does not offer it still served by
NewReno (QEMU's own TCP does not).

### T3. Assignment and the remaining operators in `$(( ))` (S)

The shell's arithmetic has no assignments (`=`, `+=` and the other
compound forms, `++` and `--`), no bit operators (`& | ^ ~ << >>`) and
no `?:`, all of which POSIX lists (XCU 2.6.4, from C). Add them with C's
precedence; an assignment sets the shell variable and has its value; `?:`
and `&& ||` evaluate only the side taken. **Check:** syscheck: each
operator, precedence against the existing ones, `x=5` leaving x set,
`i++` in a `while` loop, shifts by more than 63 defined as in the shell's
64-bit integers (said so), errors (assigning to a number, `1 ? 2`).

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
11. (Found 2026-10-20, fixed: no lookup checked search permission. A 0700
    directory protected nothing in it that was readable, so `/root` hid
    nothing from anyone who knew a name. The path walk now takes the
    caller's user id; the kernel's own lookups pass root.)
12. (Done 2026-10-25.) The time of day was the CMOS clock read at boot
    plus the nanosecond clock since, never set right again: on GitHub's
    runners it passed 2 s within minutes (2026-10-22: the test then allowed
    a quarter of the uptime), and without a timestamp counter, where the
    clock counts ticks and a busy host loses some, it was 18 s behind
    after a minute. It is now compared with the CMOS clock when read, at
    most once a second, and put right beyond 1.5 s; a time someone set is
    kept as its difference from the CMOS clock. The test allows 3 s at any
    uptime, and `time_of_day_is_put_right_by_the_cmos_clock` puts the clock
    10 s out each way (a check that measures without correcting fails it).
13. A `/proc` file is made afresh for every `read()`, so a program that
    reads one in small pieces gets pieces of different moments: a process
    whose `State:` changes length between two reads shifts every line
    after it. Found 2026-10-26 when a unit test reading a status in 7-byte
    pieces lost the `Pid:` line (and found `PPid:` instead) on four CPUs;
    the tests now read a running process's status in one call, as `cat`
    and `top` do. The fix is a copy made at the first read and kept with
    the open file until it is closed (as Linux's `seq_file` does), which
    needs a place for per-open state that `vnode_ops` reads do not have.

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
        |
I1 -> I2                         done
        |
J1, J2 -> J3, J4                 done
        |
K1, K2, K3, K4                   done (in parallel)
        |
L1                               done
L2, L3, L4, L5                   done (in parallel)
        |
M1, M2, M3, M4                   done (in parallel)
        |
N1, N2, N3, N4                   done (in parallel)
        |
O1, O2, O3, O4                   done (in parallel)
        |
P1, P2, P3, P4                   done (in parallel)
        |
Q1, Q2, Q3, Q4                   done
        |
R4                               done
R1                               done
R2                               done
R3                               done
        |
S4                               done
S1, S2, S3                       done
T3, T1, T2                       next (planned, not started)
```

(Track A, once the recommended first step because everything built on
"interrupts off" as the only lock would have had to be audited again, is
done; the order above is now history plus what is next.)

## What "done" means for any item here

- Implemented, not stubbed; no placeholder left in the tree.
- Unit tests in `tests/`, ring-3 checks where programs can see the feature,
  and a scenario target if it involves a device or more than one boot.
- `make check` passes on both CPU models.
- README status table, `Documentation.md` and this file updated in the same
  commit.
