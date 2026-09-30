# BashketKernel — Repair Plan

This plan fixes the problems found in the October 2026 review. It does not add
features. The goal is that everything the README claims is true, and that the
core of the kernel is correct enough to build on.

The original learning roadmap (`plan.md`, phases 1 to 7 and the learning gates)
was removed from the tree when this plan replaced it. It is still in git:
`git show 0c3bd4d:plan.md`. References below to "the original roadmap" mean
that file.

## Progress

- **Milestone 0:** build and doc items done. The items that need a human
  (learning contract, hand-written lesson docs) are open.
- **Milestone 1:** done and verified in QEMU.
- **Milestone 2:** test build, exit status and CI done. Cleaning up test
  state leaks and mock tests is open.
- **Milestone 3:** done. Programs run in ring 3.
- **Milestone 4:** done. Blocking, copy-on-write fork, exec, wait, kill, file
  descriptors, pipes and a user-space shell all work.
- **Milestone 5:** pipes, priority scheduling, FPU/SSE, `/dev` and `/proc`, the
  terminal, the virtio disk and FAT32 are implemented. The rest of the table below is open, and deleting code is a
  decision left to the project owners.
- **Added beyond the original plan:** safe-point kill and Ctrl-C, CPU
  protections (SMEP, SMAP, UMIP), on-demand stack growth, a terminal line
  discipline, low-memory and fuzz test targets. See "What is
  still wrong" at the end for the known gaps.

## Where things stand

- **Real and working:** boot, console, serial, GDT/IDT, PMM, VMM, heap, context
  switch, round-robin scheduler, VFS tree, keyboard, kernel shell.
- **Broken but hidden:** syscall entry, timer rate, PMM sizing, locking.
- **Mock code presented as complete:** userspace, fork/exec/wait/signals,
  virtio-blk, FAT32, virtio-net, sockets, SMP, swap, priority scheduling, FPU
  state, "custom bootloader", initrd loading, auth, hrtimer, window manager.

## Rules for this plan

1. One milestone at a time, in order. Each one ends with a check that is run
   and passes, not with "it boots".
2. No new subsystems until Milestone 4 is done.
3. A feature is either real, or labelled as a stub in the README, or deleted.
   Nothing that returns a hardcoded value is described as implemented.
4. The learning gates in section 0 of the original roadmap apply again. See
   Milestone 0.

---

## Milestone 0 — Honest docs and a reproducible build

Nothing here touches kernel behaviour. Do it first so every later change is
measured against a true baseline.

- [x] **README status table.** Replace "All 38 Phases Completed" with a table
      marking each phase `real`, `partial` or `stub`. Remove "production-grade"
      from the crypto entry. Fix the test count (it is 420, not 342), or drop
      the number.
- [x] **Documentation.md.** For each section, remove concepts the listed files
      do not contain (blocking pipes, priority aging, `fxsave`/`fxrstor`,
      password hashing, alpha-blended cursor, virtqueues, cluster chains), or
      mark the section "not implemented yet".
- [ ] **Learning contract.** `.agents/AGENTS.md` line 42 tells agents to skip
      the Explain-Back and Delete-and-Rederive gates. Decide whether that still
      stands. Recommended: remove it and restore "one phase at a time, ask
      before building later-phase infrastructure".
- [ ] **Write `docs/00-toolchain.md`** (currently empty) by hand: the boot
      chain from power-on to `_start`, and why each compiler flag exists.
- [ ] **Start `docs/ai-was-wrong.md`.** The bugs in Milestones 1 and 3 are the
      first entries: write each one up as wrong code, right code, source. The
      timer bug in 1.1 is a good first one, because the first diagnosis of it
      was also wrong.
- [x] **Fetch Limine in the build.** `limine/` is gitignored and nothing
      downloads it, so a fresh clone cannot run `make iso`. Add a Makefile rule
      that clones a pinned binary release tag, and document the version.
- [x] **Header dependencies.** Add `-MMD -MP` to `CFLAGS` and
      `-include $(OBJFILES:.o=.d)` so editing a header rebuilds its users.
- [x] **Remove the duplicate `src/.vscode/`.**

**Check:** a fresh `git clone` followed by `make run` boots, with no manual
steps. Touching `src/mm/vmm.h` rebuilds every file that includes it.

---

## Milestone 1 — Fix the core that already exists

### 1.1 Timer: one source, one known rate — done

Problem: the kernel counted about 255 ticks per second while `uptime` and
`SYS_SLEEP` assume 100.

What was actually wrong (found by tracing QEMU's interrupt controllers, not by
reading the code):

- The LAPIC timer was started uncalibrated on vector 32, the same vector as
  the PIT. That added roughly 55 ticks per second.
- Every PIT period was delivered twice. The PIT was in mode 3 (square wave);
  under QEMU its output line is re-asserted while already high, and QEMU's
  I/O APIC delivers an edge-triggered interrupt on every assertion.
- The first diagnosis blamed I/O APIC pins 0 and 2 both being routed to
  vector 32. That routing was wrong but harmless on QEMU, which never signals
  pin 0.

- [x] PIT programmed in mode 2 (rate generator), one rising edge per period
      (`src/kernel/time/pit.c`).
- [x] ISA IRQs are routed through the pin, polarity and trigger mode given by
      the ACPI MADT (`src/arch/x86_64/acpi.c`, `ioapic_route_isa_irq`). On QEMU
      the MADT wires IRQ 0 to GSI 2.
- [x] The LAPIC timer is no longer started. It needs calibration against the
      PIT and its own vector before it comes back.
- [x] `TIMER_HZ` replaces the literal `100` in the shell and `nanosleep`.

**Check (passed):** two `uptime` calls 10.5 seconds apart differ by 1047 ticks.

Known leftover: ticks are still lost while interrupts are off for a long time.
`kprintf` holds interrupts off while it draws to the framebuffer, so a burst of
boot logging drops ticks. Fix by buffering console output, or by taking
wall time from a counter instead of counting interrupts.

### 1.2 PMM: size from usable memory — done

Problem: the bitmap is sized from the highest address of any memory map entry,
including reserved regions near 1 TiB. A 2 GB VM reports 1,048,576 MB total and
allocates a 32 MB bitmap, which is scanned from frame 1 on every allocation.

- [x] Compute `highest_addr` from `USABLE` and `BOOTLOADER_RECLAIMABLE` entries
      only (`src/mm/pmm.c:37-45`).
- [x] Keep a "next free" hint so allocation does not rescan from frame 1
      (`pmm.c:151`).
- [x] Delete the kernel and boot-stack reservation blocks that guess physical
      addresses with `& 0x7FFFFFFF` (`pmm.c:108-138`). They reserve the wrong
      frames, and they are unnecessary: Limine does not mark kernel or module
      memory as usable. Write down why in the lesson doc.
- [x] Make `pmm_free_frames` report a double free instead of ignoring it.

**Check (passed):** `-m 2G` reports 2047 MB and `-m 512M` reports 511 MB.
`pmm_exhaustion_and_recovery` in `tests/test_pmm.c` allocates every frame, gets
NULL, frees them all and finds the free count restored.

### 1.3 Locking under preemption — done

Problem: the timer interrupt can switch tasks at any instruction. The heap,
PMM bitmap, `kprintf`, the run queue and the keyboard buffer have no
protection. It works today only because two tasks exist.

- [x] Add `spinlock_acquire_irqsave` / `spinlock_release_irqrestore`, which
      save RFLAGS, disable interrupts, then take the lock.
- [x] Use it in `kmalloc`/`kfree`, `pmm_alloc_frames`/`pmm_free_frames`, the
      slab allocator, `kprintf`, and the keyboard and serial ring buffers.
- [x] Disable interrupts across run queue changes and `ctx_switch` in
      `sched_yield`. The saved RFLAGS already restores them per task.
- [x] Merge the two PID counters (`sched.c:18` and `process.c:17`).
- [x] Reap zombie tasks: free the task struct and its kernel stack.

**Check (passed):** `heap_stress_under_preemption` in `tests/test_sched.c` runs
four tasks that each do 20,000 random `kmalloc`/`kfree` calls and never yield,
then confirms no block was corrupted and the heap's allocated-bytes counter is
back to its starting value.

Still to do by hand (break-it): comment out the lock in `kmalloc`/`kfree`,
predict what the test will report, and run it.

### 1.4 Boot stack — done

- [x] Replace the inline `mov rsp` in `_start` (`main.c:134-136`) with Limine's
      stack size request. Changing RSP inside a compiled C function only works
      while the compiler happens not to use the old frame.

### 1.5 Fault safety net — done

- [x] Give the double fault handler its own stack through IST1 in the TSS, so a
      kernel stack overflow prints a report instead of triple-faulting.
- [x] Add an unmapped guard page below each task's kernel stack. This means
      mapping stacks into kernel virtual space instead of using HHDM addresses.

**Check (passed):** `crash stack` in the shell prints a double fault report
with "KERNEL STACK OVERFLOW" and halts, instead of resetting QEMU. `crash pf`
and `crash div0` report vector 14 and vector 0.

Not covered: PID 0 still runs on the Limine boot stack, which has no guard
page.

---

## Milestone 2 — Tests that mean something

- [x] **Separate test build.** `make test` builds with `-DKERNEL_TESTS`, runs
      the tests, and exits QEMU through `isa-debug-exit` with a pass/fail
      status. `make run` boots straight to the shell without running tests.
- [ ] **No leaked state.** Tests currently rename PID 0 to `sh`, leave zombie
      tasks, re-initialise swap, and create `readme.txt` in the live VFS. Each
      test cleans up after itself.
- [ ] **Remove tests that assert a mock.** `test_swap`, `test_process`,
      `test_storage`, `test_net`, `test_boot` and similar only prove that the
      placeholder returns its placeholder. Delete them with the stubs, or mark
      them as stub tests in the output.
- [ ] **Add fault tests.** A deliberate divide by zero and a deliberate
      not-present page access, each confirming the right vector and CR2. These
      are the "break-it" exercises from the original roadmap. The shell
      command `crash div0|pf|stack` already triggers them by hand.
- [x] **CI.** A GitHub Actions job that runs `make test` headless on each push
      (`.github/workflows/test.yml`). Written but not yet run on GitHub.

**Check (passed):** `make test; echo $?` prints 0 on success and 2 when one
`EXPECT` is deliberately broken.

Also added: `tools/qemu_shell.py` drives the kernel shell over serial for
behaviour checks, and `tools/symbolize.sh` maps addresses to source lines.

---

## Milestone 3 — Phase 5 for real: one program in ring 3 — done

Everything after phase 4 depends on this, and none of it works yet. The target
is one small user program, built from source, that prints through a `write`
syscall and exits.

### 3.1 GDT and STAR

Problem: `sysretq` loads CS from `STAR[63:48] + 16` and SS from
`STAR[63:48] + 8`. With the current base of `0x18` and user code at `0x18`,
that loads the TSS descriptor as CS.

- [x] Reorder the GDT: `0x08` kernel code, `0x10` kernel data, `0x18` user
      data, `0x20` user code, `0x28` TSS (`src/arch/x86_64/gdt.c:65-71`).
- [x] Set `STAR[47:32] = 0x08` and `STAR[63:48] = 0x10` (`syscall.c:43`).
- [x] Verify both rules against the Intel SDM entries for SYSCALL and SYSRET,
      and record it in `ai-was-wrong.md`.

### 3.2 Syscall entry

Problems: the entry stub stays on the user's stack, and it pushes registers in
the reverse order of `struct registers`, so `regs->rax` reads the user's r15.

- [x] On entry, save the user RSP and load the current task's kernel stack. On
      a single CPU a pair of global variables is enough; move to `swapgs` and
      per-CPU data only when real SMP arrives.
- [x] Push registers so the frame matches `struct registers` exactly, or define
      a separate `struct syscall_frame`. Add a `_Static_assert` on the offsets.
- [x] Add direction flag (bit 10) to `SFMASK` alongside the interrupt flag.
- [x] Before `sysretq`, check that the return RIP is a canonical user address.

### 3.3 Address spaces and entering user mode

Problem: every user mapping goes into the single kernel PML4.

- [x] `vmm_create_address_space()`: a new PML4 whose upper 256 entries are
      copied from the kernel's. `vmm_destroy_address_space()` frees the lower
      half and its frames.
- [x] Make `elf_load`, the user stack setup and `mmap` take the target PML4
      instead of passing NULL.
- [x] Call `tss_set_rsp0` on every context switch. It is never called today.
- [x] Enter ring 3 for the first time with an `iretq` frame: CS `0x23`,
      SS `0x1B`, RFLAGS `0x202`.
- [x] `munmap` frees the frames it unmaps (`src/mm/mmap.c:60`).

### 3.4 A real user program and a real initrd

- [x] Create `user/` with a tiny `crt0`, a syscall wrapper and `hello.c`,
      linked at a low address with its own linker script.
- [x] Pack `user/` binaries into a USTAR archive at build time, load it as a
      Limine module, and call `initrd_mount` at boot. The parser exists but is
      never called.
- [x] Delete the mock ELF headers in `src/kernel/proc/user_apps.c`.

### 3.5 Do not trust user input

- [x] `elf_load` takes the file size and rejects any `e_phoff`, `p_offset` or
      `p_filesz` that points outside the file, and any `p_vaddr` in the kernel
      half (`src/fs/elf.c:30-62`).
- [x] Zero the whole of each loaded segment, including segments with
      `p_filesz == 0`.
- [x] Add `copy_from_user` / `copy_to_user` that validate the range is in the
      user half and mapped. Use them for every pointer argument in
      `syscall_handler`.
- [x] An exception that arrives from ring 3 kills that task. Only a kernel-mode
      exception halts the machine (`src/arch/x86_64/isr.c:75`).
- [x] Remove the IDT's DPL 3 gate for vector `0x80` unless it is used.

**Checks (all passed, and all are now automated in `make test`):**
1. `exec /bin/hello one two` prints from ring 3 with its arguments and the
   shell prompt returns.
2. `/bin/fault cli` is killed with a #GP report and the shell survives.
3. `/bin/fault kread` and `kwrite` are killed with a page fault report. So are
   `null`, `text` (write to code), `exec-stack` (NX) and `int`.
4. `/bin/syscheck` passes kernel pointers, wrapping lengths and read-only
   buffers to system calls and gets `-EFAULT` each time.
5. Still to do by hand: delete-and-rederive the syscall entry path, per the
   original roadmap.

Differences from the plan as written:
- The syscall frame uses the same `struct registers` layout as interrupts,
  rather than a separate struct. That is what makes `fork` a frame copy.
- `vmm_init` now creates all 256 kernel-half PML4 entries up front, so every
  address space shares the same kernel page tables for good.
- The single-CPU stack switch uses two globals (`syscall_user_rsp`,
  `syscall_kernel_rsp`). SMP will need `swapgs` and per-CPU data.

---

## Milestone 4 — Real process lifecycle

- [x] **Blocking.** `sched_block(channel, wake_tick)` and
      `sched_wakeup(channel)` (the xv6 sleep/wakeup design, chosen over linked
      wait queues because a killed sleeper leaves nothing to unlink). Used by
      `SYS_SLEEP`, `SYS_READ`, `waitpid`, `sem_wait`, `nanosleep` and the
      shell input loop. The CPU halts when nothing is runnable.
- [x] **`exit` and `wait`.** Exit status is stored, the parent is woken,
      `waitpid` blocks and reaps, and the address space is destroyed.
- [x] **`exec`.** Replaces the current task's address space and jumps to the
      new entry point, with `argv` on the user stack.
- [x] **`fork`, full copy.** Works, including a child that execs.
- [x] **`fork`, copy-on-write.** `vmm_clone_user_space` shares every frame and
      marks writable pages read-only + `VMM_COW` in both processes; the page
      fault handler copies on the first write (`vmm_handle_cow_fault`).
- [x] **File descriptors.** Per-process table of reference-counted open files
      (`src/fs/file.c`) over VFS nodes, pipe ends and the console. `open`,
      `close`, `read`, `write`, `lseek`, `pipe`, `dup2`, `readdir`, `mkdir`,
      `unlink`. Files are closed when a process dies, not when it is reaped.
- [x] **`brk` and the mmap cursor** become per-process (`syscall.c:20`,
      `mmap.c:12`).
- [x] **Userspace shell.** `/bin/sh` (`user/sh.c`) runs pipelines and
      redirections using only system calls. The kernel-thread shell stays as
      the boot shell because it has the debugging commands (`crash`, `mem`,
      `ps`) and the test driver talks to it. Booting straight into `/bin/sh`
      as an init process is the remaining step.

**Check:** a user program forks, the child execs `/bin/hello`, and the parent
prints the child's exit status after `waitpid`. Run it 1000 times in a loop and
confirm free frame count and heap usage return to their starting values.

---

## Milestone 5 — Deal with the stubs

For each item, pick one: implement it properly, or delete the code, its test
and its README entry. Suggested calls, in the order worth doing:

| Subsystem | Suggestion |
|---|---|
| Pipes | **Done.** Blocking, EOF, EPIPE, behind file descriptors. |
| Priority scheduling | **Done.** Weighted time slices; nothing starves. |
| FPU/SSE | **Done.** FXSAVE area per task, switched eagerly. |
| `/dev`, `/proc` | **Done.** Device nodes with their own read/write; `/proc` generated on read. |
| TTY | **Done.** Line discipline, raw mode owned by a process, arrow keys. `/bin/edit` is a full-screen editor built on it. One terminal; no job control. |
| virtio-blk | **Done.** Legacy PCI transport, interrupt-driven, `/dev/vda`. One request in flight at a time; no modern (MMIO capability) transport. |
| FAT32 | **Done**, read and write, mounted at `/disk`, checked against `fsck.fat` and damaged images. Open: long file names, rename, timestamps, reading directories on demand instead of all at mount time, and any crash consistency beyond write ordering. |
| SMP | Defer. Needs per-CPU data, `swapgs` and real locks: today "interrupts off" is the lock in the scheduler, pipes, semaphores and the wait path. |
| virtio-net, sockets | Defer or delete. |
| Swap | Delete until a real block device and page reclaim exist. |
| ANSI | **Done.** Escape sequence parser, acted on by the console. |
| Window manager, auth, mouse | Delete or leave clearly marked as stubs. |
| `boot_loader.c` | Delete. It is a CPUID check, and Limine boots the kernel. |
| hrtimer | `nanosleep` now blocks properly but has tick resolution. Rebuild on a calibrated LAPIC timer or fold into `sched_sleep_ticks`. |
| Environment | **Done** as a per-process, user-memory environment passed through `execve`. `src/kernel/proc/env.c` (the kernel shell's global table) can now be deleted or reduced to that shell's private use. |
| Shared memory, mount table | Each is a global kernel table no process can reach. Make them system-call features or delete. |
| Crypto library | Keep in `lib/` as a separate library. Nothing in the kernel calls it and it has not been audited. |

Add QEMU flags to `make run` as the real drivers land, for example
`-drive file=disk.img,if=virtio,format=raw` and later `-smp 2`.

---

## Order and dependencies

```
0 docs/build ──> 1 core fixes ──> 2 tests ──> 3 ring 3 ──> 4 processes ──> 5 stubs
```

Milestones 0 to 2 are small. Milestone 3 is the large one and is where the
project resumes the original roadmap. After Milestone 4 the kernel matches
phases 1 to 7 of the original roadmap honestly, which that document already calls a
success.

---

## What is still wrong

Known gaps in the parts that are otherwise real, most important first.

1. **One CPU only, by construction.** Disabling interrupts is used as the
   lock in many places. Starting a second CPU would break them all.
2. **SMAP windows are coarse for `read` and `write`.** The user access
   window is open for the whole of `file_read`/`file_write`, including while
   the task sleeps in a pipe or the terminal, rather than only around the
   final copy. Narrowing it means passing "this is a user buffer" down to the
   copy sites.
3. **A fault between `pop rsp` and `sysretq` would run on the user stack.**
   Nothing can fault there today (interrupts are off and NMIs are not used),
   but an NMI handler would need its own IST stack before one is enabled.
4. **No permissions.** Any program can delete or overwrite any file, and kill
   any user process.
5. **Signal handling is basic.** There is no signal mask (a handler can be
   interrupted by another signal, including the same one), no `sigaction`
   flags, no automatic restart of an interrupted call, no `SIGCHLD`, and no
   job control.
6. **Orphans are not re-parented.** A process whose parent has exited is
   reaped by the idle loop when it exits; nobody collects its status. A
   blocked orphan stays until it is killed.
7. **`mmap` and `brk` memory is committed eagerly.** Only `fork` (copy-on-write)
   and the user stack (grows on demand to 8 MiB) are lazy. The stack grows
   only on faults taken by the program itself: a system call given a buffer
   below the mapped part of the stack returns `EFAULT`.
8. **A killed process can leak a small kernel buffer** if it dies inside a
   system call that was holding one (`/proc` reads allocate a page).
9. **Files outside `/disk` are RAM only** and limited only by free memory.
   On `/disk`, a crash in the middle of a change can leave lost clusters
   (there is no journal), reads take no lock and can see a concurrent write
   half done, and a killed process can leak the kernel buffer of a write in
   progress.
10. **A few timer ticks are lost under heavy console output** (measured: under
   1% over 200 s of continuous scrolling), because drawing happens with
   interrupts off. Uptime is therefore slightly slow, never fast.
11. **PID 0 runs on the boot stack**, which has no guard page.
12. **`kmalloc` and `kfree` are O(number of blocks)**; fine for now, slow with
    thousands of live allocations.
13. **The GitHub Actions workflow has never been run.**
