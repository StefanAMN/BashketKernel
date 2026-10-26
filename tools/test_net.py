#!/usr/bin/env python3
"""Exchange real UDP datagrams and TCP streams between the host and the kernel in QEMU.

    tools/test_net.py [--iso build/BashketKernel.iso] [extra qemu_shell.py arguments]

Two directions, both without any network beyond the machine itself:

  guest -> host   QEMU's user-mode network delivers what the guest sends to
                  10.0.2.2 to the host's own 127.0.0.1. A listener here
                  answers /bin/udpsend, and the guest must print the answer.

  host -> guest   QEMU forwards a host UDP port to port 7 in the guest, where
                  /bin/udpecho runs. Datagrams sent here must come back
                  unchanged, one of them 8 KiB: fragmented in both
                  directions.

The same two directions over TCP:

  guest -> host   /bin/nc in the guest connects to a listener here and sends
                  it a file (its own /bin/sh). Every byte must arrive, and
                  the guest must print what the listener answers.

  host -> guest   QEMU forwards a host TCP port to port 7 in the guest, where
                  /bin/tcpecho runs. Two megabytes sent here must come back
                  unchanged.

  names           ping gateway and host GateWay must find 10.0.2.2 in
                  /etc/hosts, without asking a DNS server.

Then a second boot, on a network whose addresses are not the ones the kernel
starts with: /bin/dhcp must obtain an address from QEMU's DHCP server and the
machine must work with it; /bin/host must get an answer from a DNS server
(a small one in this script, so that no real name service is needed). The
lease is renewed twice (dhcp -d -t 1 -n 2), and QEMU's capture of the
traffic must show each renewal as a REQUEST sent to the server alone,
naming the address held, and answered with an ACK.

Exit status 0 if all of it worked.
"""

import os
import socket
import struct
import subprocess
import sys
import tempfile
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


def dns_reply(query, known):
    """Answer one DNS question: the address in `known`, or "no such name"."""
    if len(query) < 17:
        return None
    labels, pos = [], 12
    while pos < len(query) and query[pos] != 0:
        length = query[pos]
        labels.append(query[pos + 1:pos + 1 + length].decode("ascii", "replace"))
        pos += 1 + length
    question = query[12:pos + 5]
    address = known.get(".".join(labels))
    header = query[:2] + (b"\x81\x80" if address else b"\x81\x83") + b"\x00\x01" + \
        (b"\x00\x01" if address else b"\x00\x00") + b"\x00\x00\x00\x00"
    answer = b""
    if address:
        answer = b"\xc0\x0c\x00\x01\x00\x01\x00\x00\x00\x3c\x00\x04" + bytes(int(part) for part in address.split("."))
    return header + question + answer


def udp_packets(pcap_path):
    """The UDP/IPv4 packets of a pcap file written by QEMU's filter-dump:
    (source ip, destination ip, source port, destination port, payload)."""
    with open(pcap_path, "rb") as f:
        data = f.read()
    packets, pos = [], 24                       # After the file header
    while pos + 16 <= len(data):
        length = struct.unpack_from("<I", data, pos + 8)[0]
        frame = data[pos + 16:pos + 16 + length]
        pos += 16 + length
        if len(frame) < 42 or frame[12:14] != b"\x08\x00" or frame[23] != 17:
            continue                            # Not IPv4, or not UDP
        header = (frame[14] & 0x0F) * 4
        udp = frame[14 + header:]
        source_port, destination_port = struct.unpack_from(">HH", udp, 0)
        packets.append((socket.inet_ntoa(frame[26:30]), socket.inet_ntoa(frame[30:34]),
                        source_port, destination_port, udp[8:]))
    return packets


def dhcp_type(payload):
    """The DHCP message type (option 53) of a payload, or None"""
    pos = 240
    while pos + 1 < len(payload) and payload[pos] != 255:
        if payload[pos] == 0:
            pos += 1
            continue
        if payload[pos] == 53:
            return payload[pos + 2]
        pos += 2 + payload[pos + 1]
    return None


def unicast_renewals(pcap_path, client, server):
    """How many REQUESTs went from `client` to `server` alone (not broadcast),
    naming `client` as the address held, and were answered by an ACK"""
    packets = udp_packets(pcap_path)
    answered = 0
    for source, destination, source_port, destination_port, payload in packets:
        if (source, destination, source_port, destination_port) != (client, server, 68, 67):
            continue
        if dhcp_type(payload) != 3 or socket.inet_ntoa(payload[12:16]) != client:
            continue
        xid = payload[4:8]
        if any(p[2] == 67 and p[3] == 68 and p[4][4:8] == xid and dhcp_type(p[4]) == 5 for p in packets):
            answered += 1
    return answered


def dhcp_and_dns(extra):
    """The second boot: an address by DHCP, a name by DNS."""
    dns_port = free_udp_port()
    server = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    server.bind(("127.0.0.1", dns_port))
    server.settimeout(0.5)
    stop = threading.Event()
    asked = []

    def serve():
        while not stop.is_set():
            try:
                query, address = server.recvfrom(512)
            except socket.timeout:
                continue
            asked.append(query)
            reply = dns_reply(query, {"kernel.test": "203.0.113.7"})
            if reply:
                server.sendto(reply, address)

    threading.Thread(target=serve, daemon=True).start()

    capture = tempfile.NamedTemporaryFile(suffix=".pcap", delete=False)
    capture.close()
    command = [os.path.join(HERE, "qemu_shell.py"), "--quiet", "--timeout", "60",
               "--net-option", "net=192.168.76.0/24", "--net-option", "dhcpstart=192.168.76.9",
               "--qemu-arg=-object", f"--qemu-arg=filter-dump,id=capture,netdev=n0,file={capture.name}"] + extra + [
        "exec /bin/dhcp show",
        "exec /bin/ping 192.168.76.2 1",        # Not reachable with the address the kernel starts with
        "exec /bin/dhcp",
        "exec /bin/dhcp show",
        "exec /bin/dhcp -d -t 1 -n 2",          # Renew twice, a second after each lease, then exit
        "exec /bin/ping 192.168.76.2 1",
        f"exec /bin/host kernel.test 192.168.76.2 {dns_port}",
        f"exec /bin/host nowhere.test 192.168.76.2 {dns_port}",
    ]
    result = subprocess.run(command, capture_output=True)
    output = result.stdout.decode("utf-8", errors="replace")
    stop.set()
    renewals = unicast_renewals(capture.name, "192.168.76.9", "192.168.76.2")
    os.unlink(capture.name)

    problems = []
    if "interface: address 10.0.2.15" not in output:
        problems.append("DHCP: the kernel did not start with its default address")
    if "dhcp: address 192.168.76.9 netmask 255.255.255.0 gateway 192.168.76.2 dns 192.168.76.3" not in output:
        problems.append("DHCP: no lease, or not the one QEMU's server gives")
    if "interface: address 192.168.76.9" not in output:
        problems.append("DHCP: the lease was not applied to the interface")
    if output.count("dhcp: renewed by the server (192.168.76.2), lease of 86400 seconds") != 2:
        problems.append("DHCP: dhcp -d did not report two renewals by the server")
    if renewals != 2:
        problems.append(f"DHCP: the capture shows {renewals} renewals sent to the server alone and acknowledged, not 2")
    if output.count("1 sent, 1 answered") != 1 or output.count("1 sent, 0 answered") != 1:
        problems.append("DHCP: the gateway should answer after the lease and only then")
    if "kernel.test has address 203.0.113.7" not in output:
        problems.append("DNS: the name was not resolved")
    if "nowhere.test: no such name" not in output:
        problems.append("DNS: a name that does not exist was not reported as such")
    if len(asked) < 2:
        problems.append("DNS: the server on the host was never asked")
    return output, problems


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
    # The last is larger than a frame: QEMU's network (BSD's IP code, which
    # we did not write) cuts it into fragments on the way in and reassembles
    # the guest's fragments on the way out
    messages = [b"hello-guest-1", b"x" * 1400, os.urandom(8192)]
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
                    data, _ = sender.recvfrom(65536)
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
        # few more than the three messages
        "!exec /bin/udpecho 7 8",
        "@wait:9",
        "^C",
        "exec /bin/ping 10.0.2.2 1",
        f'exec /bin/sh -c "cat /bin/sh | nc 10.0.2.2 {tcp_host_port}"',
        "exec /bin/tcpecho 7 1",     # Returns once it has served the host's connection
        "exec /bin/ping 10.0.2.2 1",
        "exec /bin/ping gateway 1",  # A name from /etc/hosts: no DNS server is asked
        "exec /bin/host GateWay",    # Names are compared without regard to case
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
    if output.count("1 sent, 1 answered") != 3:
        problems.append("ping no longer works after the exchanges")
    if "reply from gateway" not in output or "GateWay has address 10.0.2.2" not in output:
        problems.append("/etc/hosts: the name gateway was not found there")

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

    second_output, second_problems = dhcp_and_dns(extra)
    output += second_output
    problems += second_problems

    if problems:
        sys.stdout.write(output)
        for problem in problems:
            print("network: FAILED:", problem)
        return 1
    megabytes = TCP_ECHO_BYTES // (1024 * 1024)
    print(f"network (UDP both ways; TCP: a {len(sent_file)}-byte file out, {megabytes} MiB echoed; DHCP lease, renewed twice; DNS lookup): ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
