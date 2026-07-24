# BashketKernel — notes for Claude

x86_64 higher-half learning kernel in C, booted by Limine, developed against
QEMU. `PLAN.md` is the current work plan and tracks what is done. The README
status table says which subsystems are real, partial or stub; keep it true.

## Working rules

- Commit every verified change directly to `main` (conventional commits:
  `fix(mm): ...`, `feat(sched): ...`). Do not push.
- A change is verified when the kernel builds with no new warnings **and** a
  headless QEMU boot passes the tests. "It boots" is not the bar.
- Intel syntax everywhere (`-masm=intel`), in `.S` files and inline asm.
- Never describe placeholder code as implemented. Real, labelled stub, or deleted.
- New code gets a test in `tests/`. Tests must clean up after themselves.
- `docs/NN-*.md` lesson docs and `docs/ai-was-wrong.md` are written by the
  humans, not by an agent.

## Build and run

```
make iso          # build/kernel.elf + build/BashketKernel.iso (fetches Limine once)
make test         # separate test kernel in build-test/, exit 0 only if all pass
make run          # QEMU with serial and monitor on stdio
make run-gdb      # QEMU halted, gdb stub on :1234

make test-lowmem                # out-of-memory scenarios on a 64 MB machine (~5 s)
make fuzz                       # random hostile syscalls, 8 seeds (~80 s); FUZZ_SEEDS="3" to reproduce one
tools/qemu_shell.py --quiet 'exec /bin/hello'   # boot headless, type shell commands (<1 s)
tools/qemu_shell.py --quiet 'exec /bin/sh' 'ls /bin | wc' 'exit'   # drive the user shell
tools/qemu_shell.py --quiet '!exec /bin/spin' '^C' 'ps'            # '!' = do not wait for a prompt; ^C / ^D keys
tools/symbolize.sh 0xffffffff80004007     # address -> function and line
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

These each cost real time to discover. Add to this list only when a finding
would change how the next problem is approached.

### Debugging technique

- **Ask QEMU, do not theorise.** When interrupt behaviour is wrong, three
  cheap tools give ground truth:
  - `-serial mon:stdio`, then send `\001c` to toggle between guest serial and
    the monitor from a script. `info irq` gives per-IRQ counts to compare
    against the guest's own counters in the same run; `info pic` and
    `info lapic` show the I/O APIC redirection table, PIC masks and LVT state.
  - `-trace 'ioapic_*' -trace 'apic_*' -trace 'pic_*' -D trace.log` shows every
    line assertion and every delivery in order.
  - `-d int -no-reboot` for the exception that precedes a triple fault.
- **Measure rates against wall time.** A clock that runs at 2.5x is invisible
  until two `uptime` calls are timed with a stopwatch.
- **A wrong diagnosis can still "fix" things.** The timer bug was first blamed
  on a double I/O APIC route. Removing it changed nothing; only comparing
  guest ticks with `info irq` exposed the real cause.

### x86 and QEMU facts that bit

- **PIT mode 3 double-delivers through QEMU's I/O APIC.** In square-wave mode
  QEMU re-asserts the PIT output while it is already high, and its I/O APIC
  delivers an edge-triggered interrupt on every assertion, not on every edge.
  The 8259 does real edge detection, so the bug only appears after switching to
  the I/O APIC. Use mode 2 (rate generator, command `0x34`).
- **ISA IRQ 0 is GSI 2.** Read the MADT interrupt source overrides; never
  hard-code I/O APIC pins. QEMU maps pin 0 to pin 2 internally, which hides a
  wrong route.
- **Size physical memory from usable entries only.** Limine's memory map has
  reserved entries near 1 TiB on QEMU. Taking the highest address of *any*
  entry made a 2 GB VM allocate a 32 MB frame bitmap.
- **The kernel image and boot stack are never in `USABLE` entries.** Limine
  reports them as executable/modules and bootloader-reclaimable, so the frame
  allocator needs no special case for them. Guessing their physical address
  from the virtual one (`virt & 0x7FFFFFFF`) is wrong.
- **`sysretq` picks selectors by arithmetic:** CS = `STAR[63:48] + 16`,
  SS = `STAR[63:48] + 8`. The GDT must therefore hold user data *before* user
  code. Getting this wrong loads whatever descriptor sits 16 bytes on.
- **A fault inside `kprintf` deadlocks the fault report** if the handler takes
  the same lock. Fatal paths call `kprintf_force_unlock()` first.
- **Stack overflow needs two things to be reportable:** an unmapped guard page
  below the stack (so it faults at all) and an IST stack for the double fault
  (the page fault cannot be delivered on the overflowed stack).

### Blocking

- **Lost wakeups: check the condition and block with interrupts off, in a
  loop.** `sched_block` is entered with interrupts disabled and uses
  `sti; hlt` (atomic) when nothing else can run. Every caller follows the
  pattern in the comment above `sched_block` in `sched.c`.
- **A round-robin scan must terminate on the current task even when that task
  is not runnable.** The old loop only stopped at a READY task; the first time
  the queue head blocked with nothing else runnable it spun forever with
  interrupts off. Symptom: a hang with `IRR 32` pending and RIP inside
  `sched_yield`. Found in seconds with the QEMU monitor (`info registers`,
  `info lapic`) plus `tools/symbolize.sh`.
- **Channels instead of linked wait queues.** A sleeper records what it waits
  for; wakeup scans the task list. Killing a sleeper then needs no unlinking,
  which is where linked wait queues usually go wrong.

### Copy-on-write

- **The kernel takes COW faults too.** With CR0.WP set, ring 0 writing to a
  user buffer (`waitpid` status, `uname`, `read`) faults on a COW page like
  ring 3 would. So the fault handler resolves COW before it asks which ring
  faulted, and syscall pointer checks accept "writable or COW".
- **Mark the parent's pages read-only as well, then reload CR3.** The parent's
  TLB still holds writable entries; without the flush its writes silently land
  in frames the child shares.
- **Count the new owner before mapping it.** Then a clone that fails half way
  is cleaned up by the ordinary destroy path, with no special unwinding.
- **Last owner writes: no copy.** If the frame's count is 1, just restore the
  writable bit.

### Killing processes

- **Never tear a task down from outside.** The first `kill` marked the target
  a zombie immediately. But the target may have been preempted half way
  through a system call (updating a file, a pipe, a page table). Now `kill`
  only records the request (`kill_pending`) and wakes the target if it sleeps;
  the target ends itself at a safe point: in `sched_block`, at syscall return,
  or when an interrupt is about to return to ring 3. The last one is what
  stops a program stuck in `for(;;)`.
- **Anything that sleeps is a safe point**, because code that can sleep
  already holds no lock. That rule let `sched_block` do the check once, for
  every blocking call in the kernel.
- **Ctrl-C needs an "ignore" bit before it is usable**, otherwise it kills
  the shell along with the program. `sh` sets it for itself and clears it in
  each child before `execve`.
- A side effect worth knowing: `kill()` returning does not mean the target is
  gone yet. Wait for it before testing for the consequences.

### Terminal

- **Echo and line editing belong in the kernel, once.** While each program
  echoed its own input, `sh < script` echoed the script and `cat` showed
  nothing while typing. With a line discipline, the user library's `readline`
  is a ten-line loop that behaves the same on a terminal, a file or a pipe.
- **Build the line discipline as a function of one input character**
  (`tty_input`), with the blocking read on top. The tests then type at it
  directly, with no keyboard, no second task and no timing.
- **No lock around the terminal state, on purpose.** A reader killed while
  holding a sleeping lock would leave the terminal locked for ever. The read
  path keeps interrupts off instead and only sleeps inside `keyboard_getchar`.
- `tools/qemu_shell.py`: a line typed *into a program* (not at a prompt) needs
  the `!` prefix, or the driver waits for a prompt that will not come.

### SMAP

- **Close the window in every interrupt and exception handler** (`clac` first
  thing): ring 3 can set RFLAGS.AC itself, and an interrupt can arrive inside
  a system call's window. `iretq` restores the interrupted flags afterwards,
  so nothing needs reopening. Mask AC in `SFMASK` for the same reason.
- **AC is per task for free**, because `ctx_switch` saves RFLAGS. A task may
  sleep with its window open without opening one for anyone else.
- The existing rule "the kernel writes other address spaces through the
  direct map" meant the ELF loader, argv setup, COW copies and stack growth
  needed no changes at all.

### Demand paging

- **The page fault handler is a dispatcher.** Order matters: copy-on-write
  (write to a present page), then stack growth (page not present, address in
  the stack region), and only then "this is a real fault". Each handler must
  refuse quickly anything that is not its case, or it will hide real bugs.
- **Leave the lowest page of the stack region unmapped for good.** It is what
  turns runaway recursion into a fault at a known address.

### Running out of memory

Found by `/bin/stress` on a 64 MB machine (`make test-lowmem`); the 2 GB test
machine never gets near these paths.

- **Growth must be all-or-nothing.** `kmalloc(32 MB)` with 20 MB free used to
  map 20 MB into the heap, fail, and keep them: the machine was then out of
  memory for good. `expand_heap` now rolls back what it mapped.
- **A heap that never shrinks turns one big temporary allocation into a
  permanent loss** for user programs. `kfree` trims a large free tail.
- **Copy-on-write is a promise the kernel may be unable to keep.** If the
  copy cannot be allocated, the writer is killed (there is no error return
  from a `mov`). The same when the kernel is the writer, on the process's
  behalf: kill the process, do not panic.
- **Do not print on allocation failure.** A user program can cause thousands
  per second. Count them (`/proc/meminfo`) instead.
- What already held up without changes: `mmap`, `brk`, and a 1022-process
  fork bomb all failed cleanly and released everything, because every
  allocation path was written to undo itself on failure and checked with the
  free-frame count.

### Fuzzing from ring 3

- `/bin/fuzz` found no kernel crash in about 230,000 calls (20 seeds, also at
  48 MB). That is evidence for the "validate every user pointer with
  `vmm_user_range_ok` before touching it" rule, not proof of anything more.
- **A fuzzer has to be kept from destroying its own test bench.** There are
  no file permissions, so unrestricted `unlink`/`open(O_TRUNC)` would delete
  `/bin`; `kill` would hit the harness; `fork` would multiply. The fuzzer
  restricts exactly those arguments and leaves the rest random.
- **Logs from fuzzed programs contain NUL bytes.** `grep` then reports
  "binary file" and prints nothing, which looks like an empty log. Use
  `grep -a`.
- It needs a watchdog process per round: some random sequences legitimately
  block forever (reading a pipe whose write end the reader also holds).

### Files and pipes

- **Close a process's files when it dies, not when it is reaped.** A zombie
  holding the write end of a pipe keeps the reader waiting for end-of-file,
  and the reader's parent is often the one that would reap the zombie.
- **A pipe needs "the other end is gone" in both directions**: read returns 0
  only when empty *and* the writer closed; write fails with EPIPE when the
  reader closed. Without the second, `big | exits-early` hangs forever.
- **The shell must close its own copies of pipe ends after forking**, or no
  reader ever sees end-of-file. Same in every child before exec.
- **Never overwrite file data in place if the node does not own it.** Initrd
  files point into the boot module and test files into string constants; the
  first write copies into an owned buffer (`owns_data`).

### Testing that a test can fail

- **Mutate the kernel once to prove a new test detects the bug it is for.**
  `/bin/fptest` passed on the first run; commenting out the `fpu_restore` in
  `sched_yield` made all four workers fail, which is what shows the test is
  worth having. Do this for any test that passes first time.
- **Check test arithmetic on the host first.** The first version of that
  calculation overflowed to infinity for every seed, so every "result" was
  equal. Compiling the same function with the host gcc showed it in seconds.
- `make test | grep FAIL` also matches the compiler command lines
  (`-DKERNEL_TESTS`). Use `make test 2>&1 | tail -6`, or grep `build-test/test.log`.

### Design choices that paid off

- **One frame layout for interrupts and system calls.** `syscall_entry` builds
  the same `struct registers` the interrupt stubs build. `fork` is then a copy
  of that frame onto the child's kernel stack with RAX set to 0, and `execve`
  is a rewrite of it. No second code path to keep in sync.
- **Create all 256 kernel-half PML4 entries at boot.** Process PML4s copy
  those entries once. Because none is ever added later, every address space
  shares the same kernel page tables without any synchronisation.
- **Write into another address space through the direct map.** The ELF loader
  and the argv builder never switch CR3: they look up the frame behind a user
  address and write to it via the HHDM. This also sidesteps write protection
  on read-only segments.
- **Build the new image fully before destroying the old one** in `execve`, so
  a failed exec returns to a program that still exists.
- **Test user-facing kernel behaviour from ring 3.** `/bin/syscheck` and
  `/bin/fault` run as real processes and the kernel tests check their exit
  status. Calling `syscall_handler` from a kernel test only proved it accepted
  kernel pointers, which is exactly what it must refuse.
- **Check for leaks by counting.** Free frames and heap bytes before and after
  a workload must match once tasks are reaped. This caught nothing here only
  because the destroy paths were written against that test.

### Verifying

- **Check `make`'s exit status, not a grep for `error:`.** A linker failure
  says "undefined reference", matches nothing, and leaves the previous ISO in
  place. I then "tested" the old kernel and got confusing results.
- **QEMU's default CPU has no SMEP/SMAP/UMIP.** `make test CPU=max` (also
  `test-lowmem`, `fuzz`) exercises them. Run both models before committing
  anything that touches system calls or fault handling.

### Compiler traps in freestanding code

- **An inline-asm instruction that changes how memory may be accessed needs a
  `"memory"` clobber.** `stac; x = *user_ptr; clac` was compiled with the load
  outside the window, because nothing told GCC that `stac` and `clac` relate
  to memory. SMAP caught it on the first run under `-cpu max`; on the default
  CPU model the bug was invisible.

- **`1 / zero` does not divide.** With a constant dividend of 1, GCC emits a
  compare instead of `idiv`, so no `#DE` is raised. Make both operands volatile.
- **Assertion macros must evaluate arguments once.** `EXPECT_EQ(sched_reap(), 4)`
  called `sched_reap()` a second time to print the failure, and reported
  "expected 4, got 0" for a call that had returned 2.
- **Never move RSP inside a C function.** Use Limine's stack size request, or
  do it in assembly before any C runs.

### Concurrency

- **Locks shared with interrupt context must disable interrupts**
  (`spinlock_acquire_irqsave`). Lock order: heap or slab, then VMM, then PMM;
  `kprintf` is a leaf.
- **Preemption makes "sample, then start tasks" racy.** A task created inside a
  test may run, allocate and even exit before `task_create` returns. Compare
  state before creation with state after reaping, not snapshots in between.
- **A task cannot free its own stack.** Exit marks it a zombie; another context
  (`sched_reap` in the idle loop) frees the stack and the task struct.
