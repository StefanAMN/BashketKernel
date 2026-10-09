#!/usr/bin/env python3
"""Exchange real UDP datagrams and TCP streams between the host and the kernel in QEMU.

    tools/test_net.py [--iso build/BashketKernel.iso] [extra qemu_shell.py arguments]

Two directions, both without any network beyond the machine itself:

  guest -> host   QEMU's user-mode network delivers what the guest sends to
                  10.0.2.2 to the host's own 127.0.0.1. A listener here
                  answers /bin/udpsend, and the guest must print the answer.

  host -> guest   QEMU forwards a host UDP port to port 7 in the guest, where
                  /bin/udpecho runs. Datagrams sent here must come back
                  unchanged.

The same two directions over TCP:

  guest -> host   /bin/nc in the guest connects to a listener here and sends
                  it a file (its own /bin/sh). Every byte must arrive, and
                  the guest must print what the listener answers.

  host -> guest   QEMU forwards a host TCP port to port 7 in the guest, where
                  /bin/tcpecho runs. Two megabytes sent here must come back
                  unchanged.

Exit status 0 if all four worked.
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


def free_tcp_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


TCP_ECHO_BYTES = 2 * 1024 * 1024


def main():
    extra = sys.argv[1:]

    # --- TCP, guest -> host: receive a file, answer, close
    tcp_host_port = free_tcp_port()
    tcp_listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    tcp_listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    tcp_listener.bind(("127.0.0.1", tcp_host_port))
    tcp_listener.listen(1)
    tcp_listener.settimeout(60)
    tcp_from_guest = []

    def receive_file_from_guest():
        try:
            conn, _ = tcp_listener.accept()
        except socket.timeout:
            return
        conn.settimeout(30)
        received = bytearray()
        try:
            while True:
                data = conn.recv(65536)
                if not data:
                    break
                received += data
            conn.sendall(b"tcp-pong-from-host\n")
        except OSError:
            pass
        conn.close()
        tcp_from_guest.append(bytes(received))

    threading.Thread(target=receive_file_from_guest, daemon=True).start()

    # --- TCP, host -> guest: two megabytes through the echo server
    tcp_forward_port = free_tcp_port()
    tcp_payload = os.urandom(TCP_ECHO_BYTES)
    tcp_echoed = []

    def echo_through_guest():
        deadline = time.monotonic() + 80
        while time.monotonic() < deadline and not tcp_echoed:
            time.sleep(1)
            try:
                conn = socket.create_connection(("127.0.0.1", tcp_forward_port), timeout=5)
            except OSError:
                continue
            conn.settimeout(30)
            received = bytearray()

            def reader():
                try:
                    while len(received) < len(tcp_payload):
                        data = conn.recv(65536)
                        if not data:
                            break
                        received.extend(data)
                except OSError:
                    pass

            thread = threading.Thread(target=reader, daemon=True)
            thread.start()
            try:
                conn.sendall(tcp_payload)
                conn.shutdown(socket.SHUT_WR)
            except OSError:
                pass
            thread.join(timeout=60)
            conn.close()
            # Before the server in the guest is listening, the forwarder
            # closes the connection at once: try again
            if received:
                tcp_echoed.append(bytes(received))

    tcp_talker = threading.Thread(target=echo_through_guest, daemon=True)

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

    command = [os.path.join(HERE, "qemu_shell.py"), "--quiet", "--timeout", "150",
               "--net-forward", f"udp:127.0.0.1:{forward_port}-:7",
               "--net-forward", f"tcp:127.0.0.1:{tcp_forward_port}-:7"] + extra + [
        f"exec /bin/udpsend 10.0.2.2 {host_port} ping-from-guest",
        # A retried datagram may reach the server twice, so let it handle a
        # few more than the two messages
        "!exec /bin/udpecho 7 6",
        "@wait:9",
        "^C",
        "exec /bin/ping 10.0.2.2 1",
        f'exec /bin/sh -c "cat /bin/sh | nc 10.0.2.2 {tcp_host_port}"',
        "exec /bin/tcpecho 7 1",     # Returns once it has served the host's connection
        "exec /bin/ping 10.0.2.2 1",
    ]
    tcp_talker.start()
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
    if output.count("1 sent, 1 answered") != 2:
        problems.append("ping no longer works after the exchanges")

    tcp_talker.join(timeout=5)
    with open(os.path.join(HERE, "..", "build", "initrd", "bin", "sh"), "rb") as f:
        sent_file = f.read()
    if not tcp_from_guest:
        problems.append("TCP: the guest never connected to the host")
    elif tcp_from_guest[0] != sent_file:
        problems.append(f"TCP: the host received {len(tcp_from_guest[0])} bytes of a {len(sent_file)}-byte file, or wrong ones")
    if "tcp-pong-from-host" not in output:
        problems.append("TCP: the guest did not print the host's answer")
    if not tcp_echoed:
        problems.append("TCP: the host never got a connection through to the guest's echo server")
    elif tcp_echoed[0] != tcp_payload:
        problems.append(f"TCP: {len(tcp_echoed[0])} of {len(tcp_payload)} bytes came back from the guest, or wrong ones")
    if f"tcpecho: {TCP_ECHO_BYTES} bytes echoed" not in output:
        problems.append("TCP: the guest's echo server did not report the whole transfer")

    if problems:
        sys.stdout.write(output)
        for problem in problems:
            print("network: FAILED:", problem)
        return 1
    megabytes = TCP_ECHO_BYTES // (1024 * 1024)
    print(f"network (UDP both ways; TCP: a {len(sent_file)}-byte file out, {megabytes} MiB echoed): ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
