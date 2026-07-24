---
name: kernel-test
description: Build BashketKernel and verify a change in QEMU - run the in-kernel unit tests, drive the kernel shell with scripted commands, or check that a deliberate fault is reported. Use after any kernel code change, before every commit, and whenever asked to run, boot or test the kernel.
---

# Verifying a kernel change

A change is verified when all three of these hold. Run them in this order.

## 1. Build with no warnings

```
make -j8 iso > /tmp/build.log 2>&1; echo "make exit $?"; grep -E 'warning:|error|undefined' /tmp/build.log
```

`make exit` must be 0 and the grep must print nothing. Check the exit status,
not just the grep: a linker error ("undefined reference") has no `error:` in
it, and if the link fails the old ISO is still there, so the next QEMU run
silently tests the previous kernel.

Any warning is a failure: the tree builds clean, so a new warning is yours.

## 2. Unit tests

```
make test 2>&1 | tail -8
```

Do not `grep FAIL` the make output: the compiler command lines contain
`-DKERNEL_TESTS` and will match. To see failures: `grep -n '\[FAIL\]' -A1 build-test/test.log`.

Builds a separate test kernel in `build-test/` (with `-DKERNEL_TESTS`), boots
it headless, and exits 0 only if the kernel reported every expectation passed.
It prints `[FAIL]` lines, any `CPU EXCEPTION`, and the summary. Full serial
output is in `build-test/test.log`. Takes about 10 seconds.

Tests live in `tests/test_<area>.c`. To add one: write `TEST(name) { ... }`
with `EXPECT_EQ` / `EXPECT_TRUE`, add `RUN_TEST(name)` to that file's suite
function, and if the file is new, declare and call its suite in
`tests/test_runner.c`. Tests run with preemption on, so never assume a task
you created has not run yet.

## 3. Behaviour in the normal kernel

`tools/qemu_shell.py` boots `build/BashketKernel.iso` and types commands into
the kernel shell, waiting for the prompt each time. A boot plus a few commands
takes under a second.

```
tools/qemu_shell.py --quiet 'ps' 'mem'
tools/qemu_shell.py --quiet --expect 'Hello from ring 3' 'exec /bin/hello a b'
tools/qemu_shell.py --mem 512M --quiet 'mem'
```

- `--expect REGEX` fails the run if the pattern is missing (repeatable).
- `--forbid REGEX` fails if it appears. `CPU EXCEPTION` and `KERNEL PANIC` are
  forbidden by default.
- `--allow-crash` for runs where the fault is the point:
  `tools/qemu_shell.py --quiet --allow-crash --expect 'KERNEL STACK OVERFLOW' 'crash stack'`
- Exit status 0 pass, 1 pattern check failed, 2 timeout (hang or no prompt).

The shell's `crash div0|pf|stack|wtext` command triggers each fault on purpose.

QEMU's default CPU model lacks SMEP, SMAP and UMIP, so on it a kernel access
to user memory outside a `user_access_begin/end` window goes unnoticed. After
any change to system calls, fault handling or anything that touches user
buffers, run the targets on both models:

```
make test && make test CPU=max
make test-lowmem CPU=max
make fuzz CPU=max FUZZ_SEEDS="1 5 7"
tools/qemu_shell.py --quiet --qemu-arg=-cpu --qemu-arg=max 'exec /bin/syscheck'
```

A missed window shows up as `CPU EXCEPTION: Page Fault ... Page-protection
violation, ... Kernel mode` with CR2 a user address; `tools/symbolize.sh <RIP>`
names the line.

### User programs

`exec <path> [args]` runs a program from `/bin` (sources in `user/*.c`) and
prints its exit status if non-zero. Arguments in "double quotes" may contain
spaces. Useful ones:

```
tools/qemu_shell.py --quiet 'exec /bin/syscheck'                 # the whole syscall interface, from ring 3
tools/qemu_shell.py --quiet 'exec /bin/sh -c "ls /bin | wc"'     # one shell command line
tools/qemu_shell.py --quiet 'exec /bin/sh' 'cat /proc/tasks' 'exit'   # interactive user shell (prompt "$ ")
tools/qemu_shell.py --quiet '!exec /bin/spin' '^C' 'ps'          # '!' = do not wait for a prompt; ^C, ^D keys
```

A user-visible kernel feature is tested from ring 3: add checks to
`user/syscheck.c` (it exits 0 only if all pass, and `make test` runs it), or
write a new `user/<name>.c` and spawn it from a kernel test with
`process_spawn` + `process_wait`, asserting on the exit status.

### Leak check

Free frames and heap bytes must return to their starting values:

```
tools/qemu_shell.py --quiet 'mem' 'exec /bin/syscheck' 'mem' | grep -E 'Free|Allocated'
```

In a kernel test: call `sched_reap()`, record `pmm_get_free_frames()` and
`kheap_get_allocated_bytes()`, run the workload, `sched_reap()` again, compare.

### Out of memory

```
make test-lowmem
```

Runs `/bin/stress mem|brk|fork|cow|cowsys|file` with 64 MB of RAM (about 5
seconds). Run it after any change to allocation, page tables, fork, the heap
or file growth. Logs are in `build/lowmem-<mode>.log`. To try one by hand:
`tools/qemu_shell.py --quiet --mem 64M 'exec /bin/stress fork' 'mem'`.

### Hostile system calls

```
make fuzz                     # 8 fixed seeds, about 80 seconds
make fuzz FUZZ_SEEDS="3"      # reproduce one seed
```

Run after changing anything a system call can reach. Logs are in
`build/fuzz-<seed>.log` and contain NUL bytes: read them with `grep -a`.
"killed" and "stopped by watchdog" rounds are normal; a failure is a kernel
exception, a hang, or `/bin/syscheck` failing afterwards.

### Does the test detect the bug?

When a new test passes on its first run, break the kernel on purpose once
(comment out the line the test is about), confirm the test fails, restore it.

## Checks that need wall-clock time

Timer rate cannot be checked from inside the guest. Compare guest ticks with
QEMU's own interrupt count (see the `kernel-debug` skill), or time two
`uptime` calls a known number of seconds apart.

## When something fails

- Timeout with no output: the kernel hung or triple-faulted before the shell.
  Re-run with `--int-log` and read `int.log`.
- `CPU EXCEPTION`: symbolize the RIP with `tools/symbolize.sh 0x...`.
- Use the `kernel-debug` skill for anything beyond that.
