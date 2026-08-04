---
name: kernel-test
description: Build BashketKernel and verify a change in QEMU - run the in-kernel unit tests, drive the kernel shell with scripted commands, or check that a deliberate fault is reported. Use after any kernel code change, before every commit, and whenever asked to run, boot or test the kernel.
---

# Verifying a kernel change

The short version: `make check` runs the unit tests, the low-memory, disk,
network, editor and login scenarios and a few fuzz seeds, on both CPU models
and on machines with one to eight CPUs (about nine minutes), and ends with
`ALL CHECKS PASSED`. Use it before every commit that touches kernel code. The
sections below are the individual steps, for when one of them fails or a
quicker, narrower check is enough while iterating.

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

Do not `grep FAIL` or `grep TESTS` the make output: the compiler command
lines contain `-DKERNEL_TESTS` and will match. Anchor the pattern
(`grep -aE '^TESTS|ALL CHECKS'`) or use `tail`. To see failures: `grep -n '\[FAIL\]' -A1 build-test/test.log`.

Builds a separate test kernel in `build-test/` (with `-DKERNEL_TESTS`), boots
it headless, and exits 0 only if the kernel reported every expectation passed.

`make test-smp` runs the same tests on four CPUs (log: `build-test/test-smp.log`)
and then real programs on 2, 4 and 8. Run it for anything that touches the
scheduler, locking, per-CPU state or page tables: a test that waits by
yielding a fixed number of times, or assumes another task has not run yet,
passes on one CPU and fails there. A failure that appears on only some runs
is a race: repeat the run a few times before believing a pass.
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

### Full-screen programs and the screen itself

A command starting with `~` types raw keys (no newline, no waiting):
`'!exec /bin/edit /tmp/f' '~hello\r^S^Q' 'cat tmp/f'`. `\r` is Enter, `\e`
Escape (`\e[A` is the up arrow), `^S ^Q ^C ^D` control keys. `make test-edit`
is the scripted editor test.

The serial log cannot show colour, the cursor, or whether the screen was
really redrawn. After changing the console, look at it: start QEMU with
`-monitor unix:m.sock,server,nowait` (a short relative path: unix socket
paths are limited to 108 bytes), send `screendump out.ppm`, convert and view.

### The network

Every run attaches a virtio network card on QEMU's user-mode network: the
guest is 10.0.2.15, and 10.0.2.2 is a simulated gateway that answers ARP and
ping and passes UDP on to the host's 127.0.0.1. Nothing needs configuring on
the host and no outside network is used.

```
tools/qemu_shell.py --quiet 'exec /bin/ping 10.0.2.2 2'
make test-net          # UDP between host and guest, both directions (tools/test_net.py)
```

`--net-forward udp:127.0.0.1:5555-:7` forwards a host port to the guest;
`@wait:5` as a command lets the guest run while something outside talks to
it; `--no-net` boots without a card. Malformed-packet handling is tested in
the kernel (`tests/test_net.c` hands frames straight to `net_handle_frame`).

### The disk

Every run attaches `disk.img` from the ISO's directory as a virtio disk
(`/dev/vda`). `tools/qemu_shell.py` and `make test` use `snapshot=on`: the
guest can write, but the image file is unchanged afterwards, so runs do not
affect each other. For a test that needs writes to last, use a scratch image:
`--disk /path/scratch.img --keep-disk-writes`. `--no-disk` boots without one.

`make test-disk` checks persistence across two boots. Raw access from the
shell: `exec /bin/diskio write 1000 "text"`, `exec /bin/diskio read 1000`.

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
