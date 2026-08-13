# BashketKernel — notes for Claude

x86_64 higher-half learning kernel in C, booted by Limine, developed against
QEMU. It is a learning-first project: the kernel is the excuse, the
understanding is the deliverable. `PLAN.md` proposes what to build next and
lists the known limits; the README status table says what exists. Keep both
true.

This file is the only rule set for agents working here (it absorbed the
former `.agents/AGENTS.md`).

## Working rules

- **Branch per part.** Pull `main` first (`git pull --ff-only`). Every part
  of the work (a plan item, a documentation pass, a fix) gets its own branch
  (`feat/...`, `fix/...`, `docs/...`), commits in conventional-commit style
  (`fix(mm): ...`, `feat(sched): ...`), one per finished piece rather than
  many tiny ones. When it is verified, merge it into `main` with `--no-ff`.
  **Push** only when asked (`origin` is
  `git@github.com:StefanAMN/BashketKernel.git`; pull requests are merged on
  GitHub too).
- **Verified** means `make check` passes (it builds, runs the unit tests and
  the scenario targets on both QEMU CPU models). "It boots" is not the bar:
  kernels routinely run while subtly broken. State the check that proves a
  change and confirm it passed.
- **No stubs.** Code is implemented and tested, or it is not in the tree.
  Never describe placeholder behaviour as a feature. The earlier stubs were
  deleted on purpose; do not reintroduce the pattern.
- **Plan before large work.** New subsystems go into `PLAN.md` first. Do not
  build later-plan infrastructure ahead of time because it is convenient;
  flag it and ask.
- **Tests.** Every new feature gets a test: in `tests/` with the in-kernel
  framework, and from ring 3 (`user/syscheck.c` or a dedicated program) when
  the feature is visible to programs. Tests clean up after themselves.
- **Every function has a comment** saying what it does: arguments, result,
  what it changes, and what the caller must know (locks, sleeping). Public
  functions once, in the header; static ones at the definition. Code that
  nothing calls is deleted, not documented (`tools/unused.sh`,
  `tools/undocumented.py`; PLAN.md track E).
- **Docs travel with the code.** Update the README status table, and the
  matching section of `Documentation.md` (the ordered guide of what to learn
  and where to read it), in the same commit as the feature.
- `docs/NN-*.md` lesson docs and `docs/ai-was-wrong.md` are written by the
  humans. If a claim of yours is corrected against a primary source, say that
  it belongs in `ai-was-wrong.md`; do not write the entry.
- The human developers learn independently: do not block work on
  explain-back or delete-and-rederive gates.

## Project constraints

- C plus small amounts of assembly; x86_64 long mode only; Intel syntax
  everywhere (`-masm=intel`), in `.S` files and inline asm.
- Bootloader is **Limine** (pinned release fetched by the Makefile; check
  `limine.conf` and the vendored `src/boot/limine.h` rather than assuming a
  protocol version). No custom bootloader.
- Kernel build flags each guard against a class of bug; do not remove or
  "simplify" them: `-ffreestanding`, `-fno-stack-protector`,
  `-fno-pie -fno-pic`, `-mno-red-zone`, `-mcmodel=kernel`,
  `-mno-mmx -mno-sse -mno-sse2`, `-nostdlib`, and the higher-half linker
  script (`0xffffffff80000000`). User programs are different on purpose:
  position-independent (`-fpie`, static PIE) and free to use SSE.
- Toolchain is the host `gcc`/`ld` (see `docs/00-toolchain.md`); do not
  switch silently.
- Test in QEMU, with the Makefile targets and `tools/qemu_shell.py`, not
  hand-rolled command lines. Use GDB on `make run-gdb` for boot and crash
  debugging rather than guessing from symptoms. Serial is the debug log; the
  framebuffer is what the user sees.
- Favour clarity over cleverness. Comment the *why*: magic numbers, bit
  layouts, hardware and ABI quirks.

## Review checklist for your own output

- Genuinely 64-bit? (register widths, `syscall`/`sysret`, 4-level paging,
  GDT layout)
- Page-table bits, I/O ports and MSR numbers taken from the Intel SDM or the
  device specification, not recalled from tutorials?
- No assumption of identity mapping: the kernel is higher-half and reaches
  physical memory through the HHDM.
- Red zone off where required; interrupts off where they must be; user
  memory touched only after validation and inside an access window?
- Table sizes exact (IDT 256 entries, GDT entry count)?
- Everything read from outside (user pointers, executables, disk, network)
  treated as untrusted and bounds-checked?
- If a well-known OSDev tutorial is the likely source of a pattern, check it
  is not reproducing one of that tutorial's known bugs.

When unsure, say so and name where to verify (Intel SDM, OSDev Wiki, Limine's
`PROTOCOL.md`, the virtio specification) instead of presenting a guess.

## Build and run

```
make iso          # build/kernel.elf, build/BashketKernel.iso (the system) and BashketKernel-dev.iso (fetches Limine once)
make test         # separate test kernel in build-test/, exit 0 only if all pass
make run          # the system: boot menu, /bin/init, a login prompt (root/root, alice/alice, bob/bob)
make run-dev      # the developer image: the kernel's own shell on the console, as the scripted tests use
make run-gdb      # the developer image in QEMU halted, gdb stub on :1234

make check                      # everything below, on both CPU models and 1 to 8 CPUs (~9 min); run before committing
make test-smp                   # unit tests on 4 CPUs, then real programs on 2, 4 and 8 (~1 min)
make test-home                  # the ext2 system disk: owners across a reboot, e2fsck, damaged images (~40 s)
make test-boot                  # the system image: init, /etc/rc, login, logout, Ctrl-C (~10 s)
make test-shutdown              # a change survives a clean power-off; the next boot skips the check; restart (~20 s)
make test-rescue                # no system disk: the kernel runs the rescue system from its boot module (~10 s)
make test-swap                  # 80 MiB used on a 64 MiB machine, with and without a swap disk (~30 s)
make test-paint                 # /bin/paint driven by the mouse, screenshots checked (~10 s)
make test-crash                 # kill QEMU during ext2 writes, reboot, e2fsck; CRASH_ROUNDS=20 for more (~1 min)
make test-login                 # login, su, passwd and file permissions, scripted (~15 s)
make test-jobs                  # user shell job control and orphan adoption, scripted (~5 s)
make test CPU=qemu64,-tsc       # no timestamp counter: the periodic-tick fallback
make test VIRTIO=legacy         # devices with only the legacy virtio interface (VIRTIO=modern: only the modern one)
make test-lowmem                # out-of-memory scenarios on a 64 MB machine (~5 s); SMP=4 for four CPUs
make fuzz                       # random hostile syscalls, 8 seeds (~80 s); FUZZ_SEEDS="3" to reproduce one; SMP=4
tools/qemu_shell.py --qemu-arg=-smp --qemu-arg=4 'exec /bin/ps'   # any scripted boot on more CPUs
tools/qemu_shell.py --quiet 'exec /bin/hello'   # boot the developer image headless, type kernel shell commands (<1 s)
tools/qemu_shell.py --iso build/BashketKernel.iso --boot-prompt 'login: ' --prompt 'login: ' --prompt 'password: ' 'root' 'root' 'id'
                                          # the system image: wait for the login prompt instead
tools/qemu_shell.py --quiet 'exec /bin/sh' 'ls /bin | wc' 'exit'   # drive the user shell
tools/qemu_shell.py --quiet '!exec /bin/spin' '^C' 'ps'            # '!' = do not wait for a prompt; ^C / ^D keys
tools/qemu_shell.py --quiet '!exec /bin/edit /tmp/f' '~hi\r^S^Q'   # '~' = raw keys into a full-screen program
tools/qemu_shell.py '!exec /bin/paint' '@button:1' '@mouse:40,0' '@button:0' '@screendump:/tmp/s.ppm' '@button:2'
                                          # '@mouse', '@button', '@screendump' go to QEMU's monitor; tools/check_ppm.py checks pixels
tools/qemu_shell.py '!exec /bin/spin' '@wait:2' '@monitor:info registers'   # where is the CPU? (any monitor command)
tools/symbolize.sh 0xffffffff80004007     # address -> function and line

# A virtio network card on QEMU's user-mode network is attached too: the
# guest is 10.0.2.15, the gateway 10.0.2.2 answers ping and reaches the host's
# 127.0.0.1. tools/test_net.py exchanges UDP with the host in both directions.
# A 64 MiB FAT disk image (build/disk.img, mounted at /disk) and the system
# disk (build/root.img: ext2, the root, made by tools/mkroot.sh from the
# programs, user/rootfs and home/) are attached to every run as virtio
# disks; --no-root-disk boots the rescue system. Scripted runs and tests
# discard their writes (snapshot=on);
# `make run` and qemu_shell.py --keep-disk-writes really modify it.
```

Two project skills in `.claude/skills/` hold the full procedures:
`kernel-test` (verify a change) and `kernel-debug` (faults, hangs, interrupts).
Use them rather than improvising QEMU command lines.

## Layout

- `src/` kernel, `lib/` freestanding libc bits and crypto, `tests/` in-kernel tests.
- `user/*.c` user programs, each built into `/bin/<name>` of the initrd;
  `user/lib` is their C library and `crt0`.
- Entry to and from ring 3: `src/arch/x86_64/syscall_entry.S`. Processes:
  `src/kernel/proc/process.c`. Address spaces: `src/mm/vmm.c`.

## Findings worth keeping

Only what would change how the next problem is approached. Details are in the
code comments and commit messages; `git log` is the full history.

### How to find things out

- **Ask QEMU instead of theorising.** `info irq`, `info pic`, `info lapic`
  and `info registers` in the monitor, `-trace 'ioapic_*'`, `-d int`, and
  `screendump` (works with `-display none`) give ground truth. The timer bug
  was first "fixed" on a wrong theory; comparing guest ticks with `info irq`
  found the real cause.
- **Use the feature before believing its status.** `dmesg` showed one line
  (nothing wrote to the log), `bench` printed PASS without timing anything,
  `date` printed a literal `%02d`, the slab allocator nothing called had four
  defects. Each had a passing test. A test ending in `EXPECT_TRUE(true)`, or
  code with no caller, is unverified.
- **Prove a new test can fail.** When a test passes first time, break the
  kernel on purpose once (comment out the line under test) and see it fail.
  Twice this exposed a test that tested nothing: one calculation overflowed to
  infinity for every input, another was hoisted out of its loop by the
  compiler (pure function, constant arguments; fixed with a volatile seed).
- **Run long, run small, run hostile.** A 200-command session found a 5x
  console slowdown; a 64 MB machine (`make test-lowmem`) found that a failed
  heap growth kept all the memory; random system calls (`make fuzz`) and
  `CPU=max` (SMEP/SMAP/UMIP, absent from QEMU's default CPU) find what unit
  tests on a 2 GB default machine cannot.
- **Check `make`'s exit status.** A linker error has no `error:` in it and
  leaves the previous ISO in place, so the next run tests the old kernel.
- **In a VM, ticks and real time disagree in both directions.** The
  calibrated clock follows the host's real time. When the host stalls QEMU,
  an interval measures long; afterwards the emulated timer delivers its
  missed ticks in a burst, so "ten ticks" measures short. A test comparing
  the two must accept one measurement in the expected window out of several.
  (An interval timed and measured by the same clock can still be given a
  strict lower bound.) Found only because the fuzzer loads the host.
- **Distrust a surprising benchmark number.** `memcpy` measured 15x slower
  than `memset` because its two buffers were exactly 1 MiB apart and collided
  in QEMU's software TLB. Change the benchmark's layout before changing code.
- **Check a format against someone else's implementation.** The FAT32 image
  builder and the FAT32 reader are both mine, so they could agree on a
  mistake; `fsck.fat` checks every image the build produces. For the other
  direction, `tools/corrupt_fat.py` damages images on purpose: loops in
  chains, a directory containing itself, garbage in the FAT, absurd sizes.
- **Judge a fuzz run by exit statuses, not by its output.** A fuzzed
  `write()` dumped a megabyte of the fuzzer's own memory to the console,
  including the string constant my check treated as the failure marker (and
  the one it treated as success). Text a hostile program can print proves
  nothing either way.
- **A root fuzzer finds its own test bench first.** Twice now a "kernel
  failure" after fuzzing was the fuzzer legitimately using root's powers:
  deleting files, then `chmod` on `/` through a random pointer that happened
  to point at the string "/". Calls that change shared state get safe
  arguments or definitely-invalid ones, never arbitrary valid memory.
- **Some defences cannot be demonstrated here.** QEMU, like AMD CPUs, faults
  in ring 3 on `sysretq` to a non-canonical address; Intel faults in ring 0.
  The check for it rests on the manual, and its test passes either way. Say
  so rather than claim the test proves it.

### x86 and QEMU facts that cost time

- **PIT mode 3 is delivered twice per period by QEMU's I/O APIC** (it
  re-asserts an already-high line and the APIC delivers on every assertion).
  Use mode 2 (`0x34`). ISA IRQ 0 is GSI 2: read the MADT, never hard-code pins.
- **Size physical memory from usable entries only**; Limine reports reserved
  entries near 1 TiB. The kernel image and boot stack are never in `USABLE`.
- **`sysretq` picks selectors by arithmetic** (CS = STAR[63:48]+16, SS = +8),
  so user data must come before user code in the GDT.
- **`swapgs` is decided by the saved CS, on the way in and on the way out
  separately.** Entry: swap if the interrupted code was ring 3. Exit: swap if
  the frame being returned to is ring 3, which is not always how we came in
  (exec, signal delivery and fork rewrite or copy frames). Loading a segment
  register resets the GS base, so the per-CPU pointer is set after the GDT
  reload, not before.
- **A TSS descriptor can be loaded once.** `ltr` marks it busy and a second
  load faults, so each CPU needs its own descriptor and its own TSS.
- **Interrupt entry clears neither DF nor AC.** The stub must `cld` (or
  compiler-generated `rep movs` runs backwards after a user `std`), and the
  handler must `clac` (ring 3 can set AC and so defeat SMAP).
- **Inline asm that changes how memory may be accessed needs a `"memory"`
  clobber.** Without it GCC moved a load of user memory outside the
  `stac`/`clac` pair. Invisible until SMAP was on.
- **With `-masm=intel`, a C identifier that is also a register name breaks
  the build.** A static variable called `fs` produced "invalid use of
  register" from the assembler, with no hint at the C source. Avoid `fs`,
  `gs`, `cs`, `ax`, `sp`, `cr3` and friends as global or static names.
- **Objects do not depend on the compiler flags unless told to.** After
  switching user programs to `-fpie`, the other build tree still held non-PIE
  objects and failed at link time with a confusing relocation error. User
  objects now depend on the Makefile.
- **`1 / zero` does not divide** (GCC emits a compare for a constant
  dividend); make both operands volatile to raise `#DE`.
- **Assertion macros must evaluate their arguments once.**

### Design rules this kernel now depends on

- **One frame layout for interrupts and system calls.** `fork` is a copy of
  the frame with RAX set to 0; `execve` and `sigreturn` rewrite it.
  `sigreturn` must leave through `iretq` (a marker in the frame selects it),
  because `sysretq` cannot restore RCX and R11.
- **Never tear a task down from outside.** `kill` records a request; the
  target ends itself at a safe point (when it would sleep, at system call
  return, or when an interrupt returns to its user code). Anything that
  sleeps holds no lock, so sleeping is always a safe point.
- **Waiting is a loop with three outcomes.** Hold the lock that protects the
  condition, check it, and call `sched_sleep(channel, &lock, deadline)`, which
  releases the lock only once the task is marked blocked (or the wakeup is
  lost). `sched_sleep` ends a killed task itself; and for a handled signal it
  returns at once, so the loop must test `sched_interrupted()` and return
  `EINTR` or it will spin.
- **Sleep on a channel, not a linked wait queue.** A killed sleeper leaves
  nothing to unlink. Likewise no sleeping lock around the terminal: a reader
  killed while holding it would lock the terminal for good.
- **Close a process's files when it dies, not when it is reaped**, or the
  reader of its pipe waits for an end-of-file that depends on the reaper.
  The same goes for its memory: a zombie is an exit status, and holds a task
  structure and a kernel stack, nothing else. (It leaves its address space
  for the kernel's before freeing it.)
- **A safe point that only interrupts reach can be starved.** A process
  short of memory spends its time in page faults; the timer almost always
  found it in the kernel, and its pending kill waited for seconds. A fault
  that has been handled and is returning to user code checks too.
- **"Done" counted by a task is not "exited".** A test that reaps "until
  something was reaped" after two tasks report in catches one of them some
  of the time. Wait for the number expected.
- **The kernel takes copy-on-write faults too** (CR0.WP), so the fault
  handler resolves COW before asking which ring faulted. If the copy cannot
  be allocated, kill the process even when the kernel was the writer.
- **Growth is all-or-nothing and every allocation path undoes itself.**
  Leak checks count free frames and heap bytes before and after; caches with
  hysteresis (heap trim, slab reserve page) must be counted as accounted for.
- **Touch user memory only inside `user_access_begin/end`, after
  `vmm_user_range_ok`.** Write other address spaces through the direct map
  (ELF loading, argv, COW copies), which needs neither.
- **Create all 256 kernel PML4 entries at boot** so every address space
  shares the kernel half with no synchronisation.
- **A loader that applies relocations is a write primitive unless it checks
  them.** Each one says "store this value at that offset"; an offset outside
  the image the loader just mapped must be refused, or a crafted executable
  gets the kernel to patch memory of its choosing.
- **Authentication is a program; the kernel only enforces numbers.** The
  stub had the kernel "logging in" users. The real design: the kernel knows
  uids and permission bits; `/bin/login` runs as root, checks the password,
  and calls `setuid`, which is one-way for anyone but root.
- **Permission checks belong where the object is reached, with the caller's
  uid passed in** (`file_open_as`), not sprinkled through system calls: the
  kernel's own callers pass root and stay unrestricted.
- **Before adding a kernel table, ask whether it is per-process data the
  kernel only carries.** The environment needed one more vector copied by
  `execve`; everything else is user-space code.
- **A mode a program sets on shared hardware must die with the program.**
  Raw terminal mode records its owner and `sched_make_zombie` gives the
  terminal back, so a killed editor cannot leave the shell unusable. The
  same hook closes files and releases the working directory.
- **Draw the cursor from the timer, not per character.** It is removed
  before any character is drawn and put back by the next refresh tick, so a
  burst of output pays for it once.
- **Echo and line editing belong in the terminal, once**, built as a function
  of one input character so tests can type at it without a keyboard.
- **Do not redraw the screen per scrolled line.** Mark it stale and refresh
  from the timer; set the flag only after the RAM copy is consistent, and
  refresh explicitly on every path that halts with interrupts off.
- **Filesystem changes are ordered data, then FAT, then directory entry**
  (the reverse when freeing), so a crash loses the newest data or leaks
  clusters but never leaves an entry pointing at someone else's clusters.
  The volume lock is held only by code that cannot fault or be killed: user
  data is copied into kernel memory before the lock is taken.
- **A VFS that keeps the whole tree in memory needs hooks, not a rewrite, to
  sit on a real filesystem**: `create`, `remove` and `truncate` on the
  directory's owner, next to the per-file `read`/`write`.
- **The thread that processes incoming packets must never wait for one.**
  The receive thread answers pings and ARP requests; if its reply needed an
  ARP answer it would wait for a packet only it can deliver. Replies sent
  from it use the sender's hardware address, learned from the packet being
  answered, and fail rather than block if that is missing.
- **PCI interrupt lines are shared.** The disk and the network card got the
  same IRQ, and a one-handler-per-IRQ table silently replaced the disk's
  handler. (The disk kept working only because of its timeout fallback: a
  safety net can hide the bug it catches.)
- **A wait during which the task owns a device must be uninterruptible**
  (`sched_sleep_uninterruptible`). A disk request in flight cannot be
  abandoned half way: a task killed there would leave the virtqueue mid-
  request and the device marked busy for good. The kill waits a few
  milliseconds for the request to finish instead.
- **Device DMA needs physical addresses of memory that stays put**: frames
  from the frame allocator, reached through the direct map, with data copied
  to and from the caller. Heap memory is neither contiguous nor identity-
  related to physical addresses.
- **"Interrupts off" is not a lock.** It keeps this CPU's interrupt handlers
  out and nobody else. Every shared structure has a spinlock (always taken
  with interrupts off) or, where the holder must sleep, a `mutex_t`. The lock
  order is the rank table in `src/kernel/sync/spinlock.h`; the kernel checks
  it on every acquire and panics at the next timer tick on a breach, in every
  build. Trust that report over reasoning: on its first run it found the
  serial receive path waking a task under a lock ranked as output. Fatal
  paths force-unlock `kprintf`, the log and the console before printing.
- **The scheduler lock is held across the task switch** and released by the
  task that comes in (a new task does it in `task_start_trampoline`). That is
  what lets another CPU free a zombie's stack the moment it sees `ZOMBIE`.
  It also means a task can never halt "in place" waiting to be woken: there
  is an idle task to switch to.
- **Keep user pointers out of everything below the system call.** `read` and
  `write` copy through a page owned by the task; the file layer, pipes and
  drivers only see kernel memory and run with the SMAP window closed. A
  buffer owned by the task (freed with it) also cannot leak when the task is
  killed while asleep in the call, which a per-call `kmalloc` would.
- **Nothing the kernel does under a spinlock may fault.** Kernel writes to
  user buffers happen under locks (a pipe's), so `vmm_user_range_ok(write)`
  resolves copy-on-write up front instead of leaving it to the fault.
- **A node pointer from a path lookup is only good under `vfs_lock()`** or
  while the node is open. Look up and pin (or look up and change) inside one
  hold of the lock.
- **Read the current task with one instruction** (`mov reg, gs:[24]`). "Find
  my CPU, then read its current task" reads another CPU's task if the task
  is moved between the two steps. For the same reason `this_cpu()` is only
  meaningful with interrupts off.
- **Control registers and MSRs are per CPU.** A second CPU needs CR0.WP,
  CR4 (SMEP, SMAP, UMIP, OSFXSR), EFER (NXE, SCE), STAR/LSTAR/SFMASK and the
  FPU set up itself, or the first user task that lands on it faults.
- **A CPU waiting with interrupts off must still answer TLB shootdowns**,
  because the CPU that asked may hold the lock it is waiting for. The request
  is a per-CPU flag, serviced from the interrupt handler *and* from every
  spin loop (lock acquire, cross-CPU call). Without the shootdown, a freed
  kernel stack slot reused at the same address was read through a stale
  translation on another CPU: a page fault within one test run.
- **"Yield N times" is not a wait on more than one CPU.** The other tasks run
  elsewhere, and N yields can be over before they have started. Tests wait
  for a condition with a time limit (`TEST_WAIT`). Likewise a process killed
  on another CPU is dead before the next statement: `kill` on an unreaped
  zombie succeeds, as in POSIX.
- **Do not inject faults on a regular beat.** "Drop every 7th TCP segment"
  fell into step with a retransmission round that happened to be 7 segments
  long (6 data, 1 acknowledgement): the same acknowledgement was lost every
  round and the connection timed out. A fixed pseudo-random sequence is
  just as repeatable and does not resonate.
- **A protocol input routine must not transmit.** A packet to ourselves is
  delivered inside the send call, so an answer sent from the input path
  nests one send inside another without limit (and each has a 1.5 KiB frame
  on a 16 KiB stack). `tcp_input` only updates state and wakes a thread that
  sends. For the same reason only one CPU at a time sends for a socket, or
  segments reach the wire in either order.
- **Test against something you did not write.** The loopback tests passed
  while data arriving before `accept` was being acknowledged and thrown
  away: both ends were ours and never sent early. The host's TCP did, on the
  first try.
- **Background teardown breaks counting tests.** A closed TCP connection
  frees its buffers two seconds later (TIME_WAIT), in whatever test happens
  to be running then. Every test now starts once the stack is quiet
  (`test_settle`), and leak checks count frames the heap holds as available
  (`test_free_frames`), since the heap grows and never shrinks.
- **Stopping is a kill that can be undone, and uses the same safe points.**
  A stop signal records a request; the process suspends itself where it
  could also have been ended. One asleep in a system call is woken to stop,
  and when continued it returns to its caller as a spurious wake-up, so the
  call carries on rather than failing with `EINTR`.
- **`fork` must leave something over.** With copy-on-write the cost of a
  fork is paid later, by whoever writes first, and a process that cannot
  get a frame then is killed mid-instruction. `fork` now fails while free
  memory is below a reserve. The low-memory test had passed for weeks by
  where the last allocation happened to land; growing a struct moved it.
- **Keys that send signals are read in interrupt handlers**, so what they
  consult (the foreground group, raw mode) is plain words, not something
  behind a lock ranked below the keyboard's.
- **Lazy memory moves the fault into the kernel unless the kernel acts
  first.** Once pages appear on first touch, a system call can be handed a
  buffer with no frames behind it, and the kernel's own access would fault
  while it holds a lock (and a file-backed page would have to sleep there).
  Every user range is populated before it is checked and used
  (`process_populate`); the fault handler serves ring 3 only.
- **A test can pin down a limitation.** One check asserted that a buffer in
  an untouched part of the stack is `EFAULT`. When that became valid the
  call succeeded and sat reading the keyboard: a hang, not a failure. A
  check of an error path should use a call that cannot block if it succeeds.
- **A phony target with no recipe succeeds.** Rewriting one Makefile target
  with a text splice deleted the next one (`test-login`); `.PHONY` still
  listed it, so `make check` went on "running" it and reporting success for
  eight commits. After editing the Makefile, list its targets
  (`grep -n '^[a-z-]*:' Makefile`) and compare with what `check` calls.
- **A set-uid program is the kernel lending rights; the program decides
  what they are used for.** The kernel's part is small: run the file as its
  owner, keep the real user id so the program can tell who called, and take
  the bit away when the file is written by anyone but root or changes
  owner. Everything else (`passwd` changing only the caller's line) is user
  code, and is where the mistakes would be.
- **A round trip is not a test of a cipher.** The library's ChaCha20 test
  encrypted, decrypted and compared: any function that is its own inverse
  passes. Before building the random generator on it, the keystream was
  checked against RFC 8439 and against a second implementation written in
  another language. The same goes for the password hashes: the kernel's
  PBKDF2 is believed because `/bin/login` accepts lines made by Python's.
- **To test what was written to disk, forget it first.** A filesystem that
  keeps its tree in memory answers every question from memory: a test that
  creates a long name and looks it up proves nothing about the entries on
  disk. The FAT tests drop a directory's nodes (`fat32_forget_directory`)
  and read it back; `fsck.fat` on the image is the outside opinion.
- **`make` cannot have prerequisites with spaces in their names** unless
  each space is escaped; a test file called "Long Names/..." made the disk
  image rule fail with "No rule to make target 'disk/Long'".
- **A slow answer to hostile input is not a hang, but check which it is.**
  Two "damaged image" cases timed out: a file whose block table pointed at
  itself and claimed 50 MiB, and one claiming 3 GiB of holes. The kernel was
  faithfully reading them. The driver was right and the test unreasonable;
  telling the two apart took looking at what the kernel was doing.
- **Reason about crash states, then pull the plug anyway.** The ext2 write
  order was designed so that a crash could only leak blocks and inodes, and
  the mount-time check was written for exactly that. The third power cut of
  the first run produced something else: blocks already listed in an
  indirect block (written at once) while the inode's size and block count
  (written last) still described the shorter file. Thirty kills found one
  class the reasoning missed; `e2fsck` is what noticed.
- **A tool that prints only at the end cannot be watched from outside.**
  `qemu_shell.py` buffers its output, so the crash test, which kills it,
  never saw the "workload started" line. Evidence that the workload ran is
  taken from the disk at the next boot instead.
- **"Out of memory" with memory free means the test before it lied.** A
  forked child was killed for a copy-on-write copy that "could not be
  allocated" with 241 frames free: the page-table entry was no longer
  present, because making room at the start of that same fault had swapped
  out the page being written. Printing the numbers at the kill took one
  run; the theories before it were all wrong.
- **A sampled RIP beats reasoning about a hang.** The swap test "hung" in
  the victim search, run in full at every fault by a process that had
  nothing to give. `@monitor:info registers` three times, then
  `tools/symbolize.sh`, said so at once.
- **CR2 is per CPU, not per task.** A fault handler that lets interrupts in
  (to wait for a disk) can be preempted, and another task's fault then
  overwrites CR2. Read it before `sti`; write it back if later code reads it.
- **When there are two ways and one is a fallback, assert which one ran.**
  The first build with the modern virtio interface booted, mounted both
  disks and pinged: on the legacy interface, because mapping the modern
  registers had failed and the code fell back. Every test would have
  passed. The boot line names the interface and each test run now refuses
  the wrong one.
- **The direct map is not a place to map device memory.** The bootloader's
  direct map already covered the device's registers with a large page, so
  `vmm_map_page` at "direct map + physical address" refused. (The local APIC
  got away with it by luck of where it sits.) Device registers go through
  `vmm_map_device`, which also maps them uncached.
- **To stop a periodic interrupt, find everything that leaned on it.** Going
  tickless took three things besides the timer: PID 0 halted in a loop and
  relied on the tick to look for dead tasks, `tcpd` woke every 50 ms whether
  or not a connection existed, and a panic on one CPU stopped the others
  "at their next tick". Each kept working and kept the CPU awake (or, the
  last, would have stopped working). `/proc/timers` is how to see it.
- **A fallback nobody runs is a stub.** "Timestamp counter unusable" had
  been in the tree untested; the first run on `-cpu qemu64,-tsc` failed five
  checks. It is in `make check` now.
- **Check that a concurrency test can fail.** Removing the test's own lock
  made the four-CPU test fail at once; putting the semaphore back on
  interrupt masking alone did not, because its window is two instructions
  wide. Such a test shows the absence of gross errors, not of races.
