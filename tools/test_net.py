#!/usr/bin/env python3
"""Exchange real UDP datagrams between the host and the kernel in QEMU.

    tools/test_net.py [--iso build/BashketKernel.iso] [extra qemu_shell.py arguments]

Two directions, both without any network beyond the machine itself:

  guest -> host   QEMU's user-mode network delivers what the guest sends to
                  10.0.2.2 to the host's own 127.0.0.1. A listener here
                  answers /bin/udpsend, and the guest must print the answer.

  host -> guest   QEMU forwards a host UDP port to port 7 in the guest, where
                  /bin/udpecho runs. Datagrams sent here must come back
                  unchanged.

Exit status 0 if both worked.
"""

import os
import socket
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))


def free_udp_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def main():
    extra = sys.argv[1:]
    host_port = free_udp_port()      # Where the guest's datagram arrives
    forward_port = free_udp_port()   # Forwarded to port 7 in the guest

    # The listener for the guest -> host direction
    listener = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    listener.bind(("127.0.0.1", host_port))
    listener.settimeout(0.5)
    from_guest = []
    stop = threading.Event()

    def answer_guest():
        while not stop.is_set():
            try:
                data, address = listener.recvfrom(2048)
            except socket.timeout:
                continue
            from_guest.append(data)
            listener.sendto(b"pong-from-host", address)

    threading.Thread(target=answer_guest, daemon=True).start()

    # The host -> guest direction: keep sending until the echo server (which
    # takes a moment to start) has answered each message
    messages = [b"hello-guest-1", b"x" * 1400]
    echoed = []

    def talk_to_guest():
        time.sleep(3)
        sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sender.settimeout(0.5)
        for message in messages:
            deadline = time.monotonic() + 12
            while time.monotonic() < deadline and not stop.is_set():
                sender.sendto(message, ("127.0.0.1", forward_port))
                try:
                    data, _ = sender.recvfrom(2048)
                except socket.timeout:
                    continue
                echoed.append(data)
                break

    talker = threading.Thread(target=talk_to_guest, daemon=True)
    talker.start()

    command = [os.path.join(HERE, "qemu_shell.py"), "--quiet", "--timeout", "40",
               "--net-forward", f"udp:127.0.0.1:{forward_port}-:7"] + extra + [
        f"exec /bin/udpsend 10.0.2.2 {host_port} ping-from-guest",
        # A retried datagram may reach the server twice, so let it handle a
        # few more than the two messages
        "!exec /bin/udpecho 7 6",
        "@wait:9",
        "^C",
        "exec /bin/ping 10.0.2.2 1",
    ]
    result = subprocess.run(command, capture_output=True)
    output = result.stdout.decode("utf-8", errors="replace")
    stop.set()
    talker.join(timeout=2)

    problems = []
    if result.returncode != 0:
        problems.append(f"qemu_shell.py exited with status {result.returncode}")
    if b"ping-from-guest" not in from_guest:
        problems.append("the host never received the guest's datagram")
    if f"answer from 10.0.2.2 port {host_port}: pong-from-host" not in output:
        problems.append("the guest did not print the host's answer")
    if echoed != messages:
        problems.append(f"the guest echoed {len(echoed)} of {len(messages)} datagrams correctly")
    if "1 sent, 1 answered" not in output:
        problems.append("ping no longer works after the exchange")

    if problems:
        sys.stdout.write(output)
        for problem in problems:
            print("network: FAILED:", problem)
        return 1
    print("network (guest to host and host to guest, over UDP): ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
