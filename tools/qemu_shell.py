#!/usr/bin/env python3
"""Boot the kernel headless in QEMU and drive its shell over the serial port.

Each command is sent only after the shell prompt appears, so there are no
fixed sleeps and the run takes as long as the kernel actually needs.

    tools/qemu_shell.py 'ps' 'mem'                # run commands, print output
    tools/qemu_shell.py --expect 'Hello' 'exec /bin/hello'
    tools/qemu_shell.py 'exec /bin/sh' 'ls /bin | wc' 'exit'   # user shell
    tools/qemu_shell.py '!exec /bin/spin' '^C' 'ps'            # interrupt a program

A command starting with '!' is sent without waiting for a prompt afterwards
(for programs that do not finish by themselves). '^C' and '^D' send Ctrl-C
and Ctrl-D. '@wait:3' lets the
guest run for 3 seconds. A command starting with '~' is typed as raw keys into a
full-screen program: '~hello\\r^S^Q' types hello, Enter, Ctrl-S, Ctrl-Q
('\\e' is Escape, so '\\e[A' is the up arrow).
    tools/qemu_shell.py --mem 512M --qemu-arg=-smp --qemu-arg=2 'uptime'
    tools/qemu_shell.py --trace-irq 'uptime'      # also write irq-trace.log

Exit status: 0 on success, 1 if an --expect pattern is missing or a
--forbid pattern (default: CPU EXCEPTION, KERNEL PANIC) appears, 2 on timeout.
With --allow-crash a fault report is expected: the run ends when the report
is complete instead of waiting for a prompt.
"""

import argparse
import os
import re
import select
import subprocess
import sys
import time

PROMPT = b"BashketOS> "
CONTROL_KEYS = {"^C": b"\x03", "^D": b"\x04", "^S": b"\x13", "^Q": b"\x11"}
USER_PROMPT = b"\n$ "  # /bin/sh, when a command starts it (e.g. 'exec /bin/sh')
CRASH_END = b"=================================================\n"


def read_until(proc, buf, patterns, deadline):
    """Read serial output until one of `patterns` appears after offset `start`."""
    start = len(buf)
    while True:
        for pat in patterns:
            # Back up a little: a pattern may have started arriving earlier
            if buf.find(pat, max(0, start - len(pat) + 1)) != -1:
                return pat
        remaining = deadline - time.monotonic()
        if remaining <= 0 or proc.poll() is not None:
            return None
        ready, _, _ = select.select([proc.stdout], [], [], min(remaining, 0.5))
        if ready:
            chunk = os.read(proc.stdout.fileno(), 65536)
            if not chunk:
                return None
            buf.extend(chunk)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("commands", nargs="*", help="shell commands to run in order")
    ap.add_argument("--iso", default="build/BashketKernel.iso")
    ap.add_argument("--mem", default="2G")
    ap.add_argument("--timeout", type=float, default=30.0, help="seconds per step (boot, each command)")
    ap.add_argument("--expect", action="append", default=[], help="regex that must appear in the output")
    ap.add_argument("--forbid", action="append", default=[], help="regex that must not appear")
    ap.add_argument("--allow-crash", action="store_true", help="a CPU exception report is the expected result")
    ap.add_argument("--trace-irq", action="store_true", help="write QEMU interrupt controller trace to irq-trace.log")
    ap.add_argument("--int-log", action="store_true", help="write QEMU -d int exception log to int.log")
    ap.add_argument("--qemu-arg", action="append", default=[], help="extra argument passed to QEMU")
    ap.add_argument("--no-net", action="store_true", help="boot without a network card")
    ap.add_argument("--net-forward", action="append", default=[], metavar="RULE",
                    help="forward a host port to the guest, e.g. udp:127.0.0.1:5555-:7 (repeatable)")
    ap.add_argument("--disk", help="disk image to attach (default: disk.img next to the ISO)")
    ap.add_argument("--no-disk", action="store_true", help="boot without a disk")
    ap.add_argument("--keep-disk-writes", action="store_true", help="let the guest really modify the disk image")
    ap.add_argument("--quiet", action="store_true", help="print only the output after the first prompt")
    args = ap.parse_args()

    cmd = ["qemu-system-x86_64", "-m", args.mem, "-cdrom", args.iso, "-boot", "d",
           "-display", "none", "-serial", "stdio", "-no-reboot"]
    # The disk image next to the ISO, if there is one. Writes are thrown away
    # when QEMU exits (snapshot=on) unless --keep-disk-writes is given, so
    # that scripted runs do not affect each other.
    disk = args.disk or os.path.join(os.path.dirname(args.iso), "disk.img")
    if not args.no_disk and os.path.exists(disk):
        snapshot = "" if args.keep_disk_writes else ",snapshot=on"
        cmd += ["-drive", f"file={disk},if=virtio,format=raw{snapshot}"]
    # A network card on QEMU's user-mode network (guest 10.0.2.15, gateway
    # 10.0.2.2). --net-forward adds host-to-guest port forwards.
    if not args.no_net:
        netdev = "user,id=n0" + "".join(",hostfwd=" + rule for rule in args.net_forward)
        cmd += ["-netdev", netdev, "-device", "virtio-net-pci,netdev=n0"]
    if args.trace_irq:
        cmd += ["-trace", "ioapic_*", "-trace", "apic_*", "-trace", "pic_*", "-D", "irq-trace.log"]
    if args.int_log:
        cmd += ["-d", "int,cpu_reset", "-D", "int.log"]
    cmd += args.qemu_arg

    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    buf = bytearray()
    status = 0
    boot_len = 0
    try:
        hit = read_until(proc, buf, [PROMPT, b"CPU EXCEPTION", b"KERNEL PANIC"], time.monotonic() + args.timeout)
        if hit == PROMPT:
            boot_len = buf.find(PROMPT)
        if hit != PROMPT:
            status = 2 if hit is None else 1
        else:
            for command in args.commands:
                if command.startswith("!"):
                    # Start something that will not return to a prompt by
                    # itself, and give it a moment to get going.
                    proc.stdin.write(command[1:].encode() + b"\n")
                    proc.stdin.flush()
                    read_until(proc, buf, [b"CPU EXCEPTION", b"KERNEL PANIC"], time.monotonic() + 0.5)
                    continue
                if command.startswith("@wait:"):
                    # Let the guest run for a while (something outside is
                    # talking to it), still watching for a crash
                    read_until(proc, buf, [b"CPU EXCEPTION", b"KERNEL PANIC"],
                               time.monotonic() + float(command.split(":", 1)[1]))
                    continue
                if command.startswith("~"):
                    # Raw keystrokes for a full-screen program: no newline is
                    # added and no prompt is awaited. "\\r" and "\\e" stand for
                    # Enter and Escape; ^S, ^Q, ^C, ^D inside are control keys.
                    keys = command[1:].replace("\\r", "\r").replace("\\e", "\x1b")
                    for name, byte in CONTROL_KEYS.items():
                        keys = keys.replace(name, byte.decode())
                    proc.stdin.write(keys.encode())
                    proc.stdin.flush()
                    read_until(proc, buf, [b"CPU EXCEPTION", b"KERNEL PANIC"], time.monotonic() + 0.4)
                    continue
                data = CONTROL_KEYS.get(command, command.encode() + b"\n")
                proc.stdin.write(data)
                proc.stdin.flush()
                hit = read_until(proc, buf, [PROMPT, USER_PROMPT, b"CPU EXCEPTION", b"KERNEL PANIC"],
                                 time.monotonic() + args.timeout)
                if hit is None:
                    status = 2
                    break
                if hit not in (PROMPT, USER_PROMPT):
                    # Let the fault report finish printing
                    read_until(proc, buf, [b"RBP:"], time.monotonic() + 3)
                    read_until(proc, buf, [CRASH_END], time.monotonic() + 2)
                    break
    finally:
        proc.kill()
        proc.wait()

    text = buf.decode("utf-8", errors="replace").replace("\r", "")
    shown = text[boot_len:] if args.quiet and boot_len else text
    sys.stdout.write(shown if shown.endswith("\n") else shown + "\n")

    forbid = list(args.forbid)
    if not args.allow_crash:
        forbid += ["CPU EXCEPTION", "KERNEL PANIC"]
    for pat in args.expect:
        if not re.search(pat, text):
            print(f"qemu_shell: MISSING expected pattern: {pat}", file=sys.stderr)
            status = status or 1
    for pat in forbid:
        if re.search(pat, text):
            print(f"qemu_shell: FOUND forbidden pattern: {pat}", file=sys.stderr)
            status = status or 1
    if status == 2:
        print("qemu_shell: TIMEOUT waiting for the shell prompt", file=sys.stderr)
    return status


if __name__ == "__main__":
    sys.exit(main())
