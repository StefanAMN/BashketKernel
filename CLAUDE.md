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

tools/qemu_shell.py --quiet 'exec /bin/hello'   # boot headless, type shell commands (<1 s)
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

### Compiler traps in freestanding code

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
