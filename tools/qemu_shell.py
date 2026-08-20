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
and Ctrl-D. '@mouse:dx,dy', '@button:n' and '@screendump:file' move the mouse,
press its buttons and save a picture of the screen (through QEMU's monitor);
'@monitor:info registers' runs any monitor command and prints its answer.
'@wait:3' lets the
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
import socket
import tempfile

PROMPT = b"BashketOS> "
CONTROL_KEYS = {"^C": b"\x03", "^D": b"\x04", "^S": b"\x13", "^Q": b"\x11", "^Z": b"\x1a"}
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
                proc.output_ended = True  # QEMU is going: see the end of main
                return None
            buf.extend(chunk)


def send(proc, data):
    """Type `data` into the guest. False if QEMU has gone (its input is closed)."""
    try:
        proc.stdin.write(data)
        proc.stdin.flush()
        return True
    except BrokenPipeError:
        return False


def monitor_command(path, command):
    """Run one command in QEMU's monitor and wait until it has been carried out."""
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as monitor:
        monitor.settimeout(10)
        monitor.connect(path)
        seen = b""
        while b"(qemu) " not in seen:            # The greeting, up to the first prompt
            seen += monitor.recv(4096)
        monitor.sendall(command.encode() + b"\n")
        seen = b""
        while b"(qemu) " not in seen:            # The echo and any output, up to the next
            seen += monitor.recv(4096)
        return seen.decode("utf-8", errors="replace")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("commands", nargs="*", help="shell commands to run in order")
    ap.add_argument("--iso", default="build/BashketKernel-dev.iso",
                    help="image to boot (default: the developer image, which starts the kernel shell)")
    ap.add_argument("--boot-prompt", default=None,
                    help="text that means the machine has booted, instead of the kernel shell's prompt "
                         "(e.g. 'login: ' for the system image)")
    ap.add_argument("--prompt", action="append", default=[],
                    help="further text that means a command has finished and the next may be typed "
                         "(e.g. 'password: ')")
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
    ap.add_argument("--net-option", action="append", default=[], metavar="OPTION",
                    help="extra option for QEMU's user-mode network, e.g. net=192.168.76.0/24 (repeatable)")
    ap.add_argument("--disk", help="disk image to attach (default: disk.img next to the ISO)")
    ap.add_argument("--no-disk", action="store_true", help="boot without a disk")
    ap.add_argument("--root-disk", help="system disk (ext2, mounted as /) to attach as the second disk (default: root.img next to the ISO)")
    ap.add_argument("--swap-disk", help="swap image (made by mkswap) to attach as a further disk; default: none")
    ap.add_argument("--no-root-disk", action="store_true", help="boot without the system disk: the kernel then runs the rescue system from its boot module")
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
        # The second disk: the system disk (ext2, mounted as /), if it was
        # built. Only together with the first: the kernel numbers disks as it
        # finds them.
        root = args.root_disk or os.path.join(os.path.dirname(args.iso), "root.img")
        if not args.no_root_disk and os.path.exists(root):
            cmd += ["-drive", f"file={root},if=virtio,format=raw{snapshot}"]
        # A swap disk, only when asked for. Never written for keeps: what
        # is on it means nothing after the machine stops.
        if args.swap_disk:
            cmd += ["-drive", f"file={args.swap_disk},if=virtio,format=raw,snapshot=on"]
    # A network card on QEMU's user-mode network (guest 10.0.2.15, gateway
    # 10.0.2.2). --net-forward adds host-to-guest port forwards.
    if not args.no_net:
        netdev = "user,id=n0" + "".join(",hostfwd=" + rule for rule in args.net_forward)
        netdev += "".join("," + option for option in args.net_option)
        cmd += ["-netdev", netdev, "-device", "virtio-net-pci,netdev=n0"]
    if args.trace_irq:
        cmd += ["-trace", "ioapic_*", "-trace", "apic_*", "-trace", "pic_*", "-D", "irq-trace.log"]
    if args.int_log:
        cmd += ["-d", "int,cpu_reset", "-D", "int.log"]
    cmd += args.qemu_arg

    # '@mouse', '@button' and '@screendump' commands go to QEMU itself, through
    # its monitor on a socket (only set up when one of them is used)
    monitor_path = None
    if any(c.startswith(("@mouse:", "@button:", "@screendump:", "@monitor:")) for c in args.commands):
        monitor_path = os.path.join(tempfile.mkdtemp(prefix="qemu-shell-"), "monitor")
        cmd += ["-monitor", f"unix:{monitor_path},server=on,wait=off"]

    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    buf = bytearray()
    status = 0
    boot_len = 0
    # What "booted" and "ready for the next command" look like
    boot_prompt = args.boot_prompt.encode() if args.boot_prompt else PROMPT
    ready = [PROMPT, USER_PROMPT] + [p.encode() for p in args.prompt]
    try:
        hit = read_until(proc, buf, [boot_prompt, b"CPU EXCEPTION", b"KERNEL PANIC"], time.monotonic() + args.timeout)
        if hit == boot_prompt:
            boot_len = buf.find(boot_prompt)
        if hit != boot_prompt:
            status = 2 if hit is None else 1
        else:
            for command in args.commands:
                if command.startswith("!"):
                    # Start something that will not return to a prompt by
                    # itself, and give it a moment to get going.
                    if not send(proc, command[1:].encode() + b"\n"):
                        break
                    read_until(proc, buf, [b"CPU EXCEPTION", b"KERNEL PANIC"], time.monotonic() + 0.5)
                    continue
                if command.startswith("@") and not command.startswith("@wait:") and proc.poll() is not None:
                    continue  # QEMU has gone (the machine switched itself off): nobody to ask
                if command.startswith("@monitor:"):
                    # Any monitor command ('@monitor:info registers'); what it
                    # prints goes to stderr, apart from the guest's output
                    sys.stderr.write(monitor_command(monitor_path, command.split(":", 1)[1]))
                    continue
                if command.startswith(("@mouse:", "@button:", "@screendump:")):
                    kind, value = command[1:].split(":", 1)
                    if kind == "mouse":          # '@mouse:40,-10': right 40, up 10
                        dx, dy = value.split(",")
                        monitor_command(monitor_path, f"mouse_move {int(dx)} {int(dy)}")
                    elif kind == "button":       # '@button:1': left down; 2 right, 4 middle, 0 none
                        monitor_command(monitor_path, f"mouse_button {int(value)}")
                    else:                        # '@screendump:shot.ppm': the screen, as a PPM file
                        monitor_command(monitor_path, f"screendump {value}")
                    # Let the guest take the interrupt and act on it
                    read_until(proc, buf, [b"CPU EXCEPTION", b"KERNEL PANIC"], time.monotonic() + 0.3)
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
                    if not send(proc, keys.encode()):
                        break
                    read_until(proc, buf, [b"CPU EXCEPTION", b"KERNEL PANIC"], time.monotonic() + 0.4)
                    continue
                data = CONTROL_KEYS.get(command, command.encode() + b"\n")
                if not send(proc, data):
                    break
                hit = read_until(proc, buf, ready + [b"CPU EXCEPTION", b"KERNEL PANIC"],
                                 time.monotonic() + args.timeout)
                if hit is None:
                    status = 2
                    break
                if hit not in ready:
                    # Let the fault report finish printing
                    read_until(proc, buf, [b"RBP:"], time.monotonic() + 3)
                    read_until(proc, buf, [CRASH_END], time.monotonic() + 2)
                    break
    finally:
        # Did the machine switch itself off (or reset, with -no-reboot)
        # before we were done with it? QEMU closes its output a moment
        # before it has exited: asked at once, the answer was "still
        # running" now and then, and it was killed instead.
        ended_by_itself = proc.poll()
        if ended_by_itself is None and getattr(proc, "output_ended", False):
            try:
                ended_by_itself = proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                pass
        proc.kill()
        proc.wait()
        try:
            proc.stdin.close()      # (Typed input still buffered for a QEMU that has gone)
        except (BrokenPipeError, OSError):
            pass

    text = buf.decode("utf-8", errors="replace").replace("\r", "")
    if ended_by_itself is not None:
        text += f"qemu_shell: QEMU exited by itself (status {ended_by_itself})\n"
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
