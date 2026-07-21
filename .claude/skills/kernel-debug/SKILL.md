---
name: kernel-debug
description: Debug BashketKernel faults and hangs under QEMU - triple faults and silent resets, CPU exception reports, wrong interrupt or timer behaviour, hangs, memory corruption. Use when the kernel crashes, hangs, resets, or misbehaves and reading the code has not explained it.
---

# Debugging the kernel under QEMU

Rule one: get ground truth from QEMU before forming a theory. Every tool
below is one command. A wrong theory about interrupts cost more time on this
project than all the tools combined.

## Start here: what kind of failure is it?

| Symptom | First tool |
|---|---|
| Fault report on serial (`CPU EXCEPTION`) | `tools/symbolize.sh <RIP>` |
| QEMU resets or exits silently (triple fault) | `--int-log` |
| Hang, no output | gdb batch backtrace |
| Timer, keyboard or IRQ behaves wrongly | monitor `info irq` / `--trace-irq` |
| Works once, corrupts later | heap stress test, guard pages |

## Symbolize an address

```
tools/symbolize.sh 0xffffffff80004007            # normal kernel
tools/symbolize.sh -k build-test/kernel.elf 0x...  # test kernel
```

The kernel is built with `-g -fno-omit-frame-pointer`, so RBP chains are
walkable and line numbers are exact.

## Triple fault: find the first exception

```
tools/qemu_shell.py --int-log 'the command that kills it'
grep -n 'v=0[0-9a-f] ' int.log | tail -20
```

Each `v=NN` line is an interrupt or exception with full register state.
Hardware IRQs are `v=20` and up; exceptions are `v=00`..`v=1f`. Read upward
from the `CPU Reset` line: the last exceptions before it are usually
page fault (0e), then double fault (08), then reset. `CR2=` on the 0e line is
the address that could not be reached, and `RIP=` says who touched it. A
`v=0d` (general protection) with `e=` set to a selector value means a bad
segment selector, typically a wrong GDT layout or a bad `iretq`/`sysretq`
frame.

## Hang: where is it stuck?

```
qemu-system-x86_64 -m 2G -cdrom build/BashketKernel.iso -boot d -display none \
  -serial file:serial.log -no-reboot -s &
sleep 5
gdb -batch -ex 'target remote :1234' -ex 'bt' -ex 'info registers rip rsp rflags' build/kernel.elf
kill %1
```

`rflags` without `IF` in a loop means spinning with interrupts off, usually a
lock taken twice. A `hlt` with `IF` clear can never wake up.

## Interrupt controllers: ask QEMU what it sees

Guest counters and QEMU's counters in the same run:

```
(sleep 3; printf 'uptime\n'; sleep 0.5; printf '\001cinfo irq\n\001c'; sleep 10; \
 printf 'uptime\n'; sleep 0.5; printf '\001cinfo irq\n\001c'; sleep 1) | \
 timeout 20 qemu-system-x86_64 -m 2G -cdrom build/BashketKernel.iso -boot d \
 -display none -serial mon:stdio -no-reboot | grep -a 'Uptime\|^ 0:'
```

`\001c` (Ctrl-A c) switches the multiplexed stdio between the guest's serial
port and the QEMU monitor. Useful monitor commands: `info irq` (counts per
IRQ), `info pic` (I/O APIC redirection table and 8259 masks), `info lapic`
(LVT entries, timer, ISR/IRR), `info registers`, `info tlb`, `info mem`,
`x/8gx <addr>`.

Every assertion and delivery, in order:

```
tools/qemu_shell.py --trace-irq 'uptime'
awk '{print $1}' irq-trace.log | sort | uniq -c | sort -rn | head
```

If `apic_deliver_irq` appears more often than the device raises its line,
something is delivering twice.

## Memory problems

- `mem` before and after an operation should match once tasks are reaped;
  a difference is a leak.
- The frame allocator prints `PMM WARNING: Freeing frame ... not allocated`
  on a double free.
- A fault address inside `0xFFFFA000...` on a 20 KiB boundary's first page is
  a kernel stack guard page: stack overflow, reported as such.
- `info tlb` and `info mem` in the monitor show what is really mapped, which
  beats reasoning about page-table code.

## Page-table and address-space layout

| Range | Use |
|---|---|
| `0x0000000000000000`..`0x00007FFFFFFFFFFF` | User space |
| `0xFFFF800000000000`.. | Limine direct map of physical memory (HHDM) |
| `0xFFFF900000000000`.. | Kernel heap |
| `0xFFFFA00000000000`.. | Kernel stacks with guard pages |
| `0xFFFFFFFF80000000`.. | Kernel image |

## After fixing

Add the cause to `CLAUDE.md` only if it would change how the next bug is
approached. Then run the `kernel-test` skill.
