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

make check                      # all of the below on both CPU models (~3 min); run before committing
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
- **Distrust a surprising benchmark number.** `memcpy` measured 15x slower
  than `memset` because its two buffers were exactly 1 MiB apart and collided
  in QEMU's software TLB. Change the benchmark's layout before changing code.
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
- **Interrupt entry clears neither DF nor AC.** The stub must `cld` (or
  compiler-generated `rep movs` runs backwards after a user `std`), and the
  handler must `clac` (ring 3 can set AC and so defeat SMAP).
- **Inline asm that changes how memory may be accessed needs a `"memory"`
  clobber.** Without it GCC moved a load of user memory outside the
  `stac`/`clac` pair. Invisible until SMAP was on.
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
- **Waiting is a loop with three outcomes.** Check the condition and call
  `sched_block` with interrupts off (or the wakeup is lost); `sched_block`
  ends a killed task itself; and for a handled signal it returns at once, so
  the loop must test `sched_interrupted()` and return `EINTR` or it will spin.
- **Sleep on a channel, not a linked wait queue.** A killed sleeper leaves
  nothing to unlink. Likewise no sleeping lock around the terminal: a reader
  killed while holding it would lock the terminal for good.
- **Close a process's files when it dies, not when it is reaped**, or the
  reader of its pipe waits for an end-of-file that depends on the reaper.
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
- **Before adding a kernel table, ask whether it is per-process data the
  kernel only carries.** The environment needed one more vector copied by
  `execve`; everything else is user-space code.
- **Echo and line editing belong in the terminal, once**, built as a function
  of one input character so tests can type at it without a keyboard.
- **Do not redraw the screen per scrolled line.** Mark it stale and refresh
  from the timer; set the flag only after the RAM copy is consistent, and
  refresh explicitly on every path that halts with interrupts off.
- **Lock order:** heap or slab, then VMM, then PMM; `kprintf` is a leaf and
  fatal paths force-unlock it. Locks shared with interrupts disable them.
