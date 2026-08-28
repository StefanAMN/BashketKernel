#include "test.h"
#include "net.h"
#include "virtio_net.h"
#include "pci.h"
#include "pit.h"
#include "heap.h"
#include "sched.h"
#include "kstring.h"
#include "virtio_pci.h"
#include "kerrno.h"

TEST(pci_finds_the_devices_qemu_provides) {
    // By type: a device that offers only the modern interface has another
    // id (0x1040 + type) than one that still offers the legacy one
    pci_device_t dev;
    EXPECT_TRUE(virtio_pci_find(VIRTIO_TYPE_BLOCK, 0, &dev));
    EXPECT_TRUE(dev.device_id == 0x1001 || dev.device_id == 0x1042);
    EXPECT_TRUE(virtio_pci_find(VIRTIO_TYPE_NET, 0, &dev));
    EXPECT_TRUE(dev.device_id == 0x1000 || dev.device_id == 0x1041);
    EXPECT_TRUE(!virtio_pci_find(VIRTIO_TYPE_NET, 1, &dev));    // One card, not two
    EXPECT_TRUE(!pci_find_nth_device(0xDEAD, 0xBEEF, 0, &dev));

    // Configuration space reads agree with what the scan recorded
    EXPECT_TRUE(virtio_pci_find(VIRTIO_TYPE_NET, 0, &dev));
    EXPECT_EQ(pci_read16(&dev, 0), 0x1AF4);
    EXPECT_EQ(pci_read16(&dev, 2), dev.device_id);
    EXPECT_TRUE(pci_read16(&dev, PCI_COMMAND) & 0x4);       // Bus mastering was switched on
}

TEST(net_checksum_known_values) {
    // The worked example of RFC 1071, section 3
    static const uint8_t rfc[] = { 0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7 };
    EXPECT_EQ(net_checksum(rfc, sizeof(rfc)), 0x220d);

    // A real IPv4 header with its checksum in place sums to zero
    static const uint8_t header[] = { 0x45, 0x00, 0x00, 0x73, 0x00, 0x00, 0x40, 0x00, 0x40, 0x11,
                                      0xb8, 0x61, 0xc0, 0xa8, 0x00, 0x01, 0xc0, 0xa8, 0x00, 0xc7 };
    EXPECT_EQ(net_checksum(header, sizeof(header)), 0);

    // An odd number of bytes: the last one counts as the high byte of a pair
    static const uint8_t odd[] = { 0x01, 0x02, 0x03 };
    EXPECT_EQ(net_checksum(odd, 3), (uint16_t)~(0x0102 + 0x0300));
    EXPECT_EQ(net_checksum(odd, 0), 0xFFFF);
}

TEST(net_card_and_stack_are_up) {
    EXPECT_TRUE(virtio_net_present());
    EXPECT_TRUE(net_up());

    uint8_t mac[6];
    virtio_net_get_mac(mac);
    static const uint8_t qemu_default[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
    EXPECT_EQ(memcmp(mac, qemu_default, 6), 0);
}

// QEMU's user-mode network answers ARP and ping for the gateway address
// itself, so this exercises the whole path in both directions: ARP request
// and reply, an ICMP echo out through the card, and the reply back in
// through the interrupt and the receive thread.
TEST(net_ping_gateway) {
    uint64_t sent_before = virtio_net_tx_count(), received_before = virtio_net_rx_count();
    uint64_t rtt = 999;

    EXPECT_EQ(net_ping(NET_GATEWAY, 200, &rtt), 0);
    EXPECT_TRUE(rtt < 100);
    EXPECT_TRUE(net_arp_known(NET_GATEWAY));                 // Learned on the way
    EXPECT_TRUE(virtio_net_tx_count() >= sent_before + 1);
    EXPECT_TRUE(virtio_net_rx_count() >= received_before + 1);

    // Several in a row: sequence numbers and buffers are reused correctly
    for (int i = 0; i < 20; i++) EXPECT_EQ(net_ping(NET_GATEWAY, 200, NULL), 0);
}

// A neighbour's address is asked for again once the answer is a minute old,
// so a machine that changes its card (or a replaced gateway) is found again.
// Until then the cached answer is used without asking.
TEST(net_arp_answers_age) {
    EXPECT_EQ(net_ping(NET_GATEWAY, 200, NULL), 0);
    uint64_t asked = net_stats()->arp_requests_sent;
    EXPECT_EQ(net_ping(NET_GATEWAY, 200, NULL), 0);
    EXPECT_TRUE(net_stats()->arp_requests_sent == asked);                 // Fresh: not asked

    EXPECT_TRUE(net_arp_age(NET_GATEWAY, 6001));                           // Older than a minute
    EXPECT_EQ(net_ping(NET_GATEWAY, 200, NULL), 0);
    EXPECT_TRUE(net_stats()->arp_requests_sent == asked + 1);             // Asked again, answered
    EXPECT_EQ(net_ping(NET_GATEWAY, 200, NULL), 0);
    EXPECT_TRUE(net_stats()->arp_requests_sent == asked + 1);             // And fresh again
    EXPECT_TRUE(!net_arp_age(NET_IP(10, 0, 2, 99), 1));                   // (Unknown: nothing to age)
}

TEST(net_ping_ourselves_and_nobody) {
    // To our own address: delivered inside the stack, without the card
    uint64_t sent_before = virtio_net_tx_count();
    uint64_t replies_before = net_stats()->echo_replies_sent;
    EXPECT_EQ(net_ping(NET_LOCAL_IP, 100, NULL), 0);
    EXPECT_EQ(net_ping(NET_IP(127, 0, 0, 1), 100, NULL), 0);
    EXPECT_TRUE(virtio_net_tx_count() == sent_before);
    EXPECT_TRUE(net_stats()->echo_replies_sent == replies_before + 2);

    // An address on our network that nobody has: ARP gets no answer. This
    // takes the full ARP timeout (three tries of 0.3 s).
    uint64_t start = timer_ticks();
    EXPECT_EQ(net_ping(NET_IP(10, 0, 2, 99), 100, NULL), -EHOSTUNREACH);
    uint64_t waited = timer_ticks() - start;
    EXPECT_TRUE(waited >= 80 && waited <= 110);
    EXPECT_TRUE(!net_arp_known(NET_IP(10, 0, 2, 99)));
}

TEST(udp_sockets_over_loopback) {
    size_t heap_before = kheap_get_allocated_bytes();
    char buf[64];
    uint32_t from_ip = 0;
    uint16_t from_port = 0;

    udp_socket_t *server = udp_open();
    udp_socket_t *client = udp_open();
    EXPECT_TRUE(server != NULL && client != NULL && server != client);

    EXPECT_EQ(udp_bind(server, 7000), 0);
    EXPECT_EQ(udp_bind(server, 7001), -EINVAL);              // Already bound
    EXPECT_EQ(udp_bind(client, 7000), -EADDRINUSE);
    EXPECT_EQ(udp_bind(client, 0), -EINVAL);

    // Client to server. The client never bound, so it is given a port, and
    // the server learns that port from the datagram.
    EXPECT_EQ(udp_sendto(client, NET_LOCAL_IP, 7000, "hello", 5), 5);
    memset(buf, 0, sizeof(buf));
    EXPECT_EQ(udp_recvfrom(server, buf, sizeof(buf), &from_ip, &from_port, 100), 5);
    EXPECT_EQ(memcmp(buf, "hello", 5), 0);
    EXPECT_TRUE(from_ip == NET_LOCAL_IP);
    EXPECT_TRUE(from_port >= 49152);

    // And the answer back to that port
    EXPECT_EQ(udp_sendto(server, from_ip, from_port, "world!", 6), 6);
    uint16_t reply_port = 0;
    EXPECT_EQ(udp_recvfrom(client, buf, sizeof(buf), NULL, &reply_port, 100), 6);
    EXPECT_EQ(memcmp(buf, "world!", 6), 0);
    EXPECT_EQ(reply_port, 7000);

    // A datagram longer than the buffer is cut, not spilled
    EXPECT_EQ(udp_sendto(client, NET_LOCAL_IP, 7000, "0123456789", 10), 10);
    memset(buf, 'x', sizeof(buf));
    EXPECT_EQ(udp_recvfrom(server, buf, 4, NULL, NULL, 100), 4);
    EXPECT_EQ(memcmp(buf, "0123xxxx", 8), 0);

    // The largest datagram goes through whole; one byte more is refused
    static uint8_t big[UDP_PAYLOAD_MAX + 1], big_back[UDP_PAYLOAD_MAX];
    for (size_t i = 0; i < sizeof(big); i++) big[i] = (uint8_t)(i * 11);
    EXPECT_EQ(udp_sendto(client, NET_LOCAL_IP, 7000, big, UDP_PAYLOAD_MAX), UDP_PAYLOAD_MAX);
    EXPECT_EQ(udp_recvfrom(server, big_back, sizeof(big_back), NULL, NULL, 100), UDP_PAYLOAD_MAX);
    EXPECT_EQ(memcmp(big, big_back, UDP_PAYLOAD_MAX), 0);
    EXPECT_EQ(udp_sendto(client, NET_LOCAL_IP, 7000, big, UDP_PAYLOAD_MAX + 1), -EMSGSIZE);

    // Nothing waiting: the timeout is honoured
    uint64_t start = timer_ticks();
    EXPECT_EQ(udp_recvfrom(server, buf, sizeof(buf), NULL, NULL, 10), -ETIMEDOUT);
    uint64_t waited = timer_ticks() - start;
    EXPECT_TRUE(waited >= 10 && waited <= 13);

    // A port nobody listens on, and a queue that fills up: both counted, both harmless
    uint64_t no_socket = net_stats()->udp_no_socket, full = net_stats()->udp_queue_full;
    EXPECT_EQ(udp_sendto(client, NET_LOCAL_IP, 7999, "x", 1), 1);
    EXPECT_TRUE(net_stats()->udp_no_socket == no_socket + 1);
    // A burst of small datagrams all fit (the queue once held eight)
    for (int i = 0; i < 100; i++) udp_sendto(client, NET_LOCAL_IP, 7000, "q", 1);
    EXPECT_TRUE(net_stats()->udp_queue_full == full);
    int drained = 0;
    while (udp_recvfrom(server, buf, sizeof(buf), NULL, NULL, 2) == 1) drained++;
    EXPECT_EQ(drained, 100);
    // Large ones fill the budget, and exactly the excess is dropped
    static char kilo[1000];
    int fit = UDP_QUEUE_BYTES / (UDP_DATAGRAM_OVERHEAD + sizeof(kilo));
    for (int i = 0; i < fit + 5; i++) udp_sendto(client, NET_LOCAL_IP, 7000, kilo, sizeof(kilo));
    EXPECT_TRUE(net_stats()->udp_queue_full == full + 5);
    drained = 0;
    while (udp_recvfrom(server, big_back, sizeof(big_back), NULL, NULL, 2) == (int64_t)sizeof(kilo)) drained++;
    EXPECT_EQ(drained, fit);
    // Empty datagrams are not free either
    full = net_stats()->udp_queue_full;
    for (int i = 0; i < UDP_QUEUE_BYTES / UDP_DATAGRAM_OVERHEAD + 1; i++) udp_sendto(client, NET_LOCAL_IP, 7000, "", 0);
    EXPECT_TRUE(net_stats()->udp_queue_full == full + 1);
    while (udp_recvfrom(server, buf, sizeof(buf), NULL, NULL, 2) == 0) { }
    // Unread datagrams are freed when the socket closes (the heap check below)
    for (int i = 0; i < 5; i++) udp_sendto(client, NET_LOCAL_IP, 7000, kilo, sizeof(kilo));

    // An unbound socket cannot receive; a closed one frees its port and memory
    udp_socket_t *unbound = udp_open();
    EXPECT_EQ(udp_recvfrom(unbound, buf, sizeof(buf), NULL, NULL, 5), -EINVAL);
    udp_close(unbound);
    udp_close(server);
    udp_close(client);
    udp_socket_t *again = udp_open();
    EXPECT_EQ(udp_bind(again, 7000), 0);
    udp_close(again);
    EXPECT_TRUE(kheap_get_allocated_bytes() == heap_before);

    // No more sockets than the table holds
    udp_socket_t *many[UDP_MAX_SOCKETS + 1];
    int opened = 0;
    while (opened <= UDP_MAX_SOCKETS && (many[opened] = udp_open()) != NULL) opened++;
    EXPECT_EQ(opened, UDP_MAX_SOCKETS);
    for (int i = 0; i < opened; i++) udp_close(many[i]);
    EXPECT_TRUE(kheap_get_allocated_bytes() == heap_before);
}

// Build an Ethernet + IPv4 + UDP frame addressed to us, correct in every
// field, so that tests can break one thing at a time.
static size_t make_udp_frame(uint8_t *frame, uint16_t port, const char *payload, size_t payload_len) {
    uint8_t mac[6];
    virtio_net_get_mac(mac);
    memset(frame, 0, 14 + 20 + 8 + payload_len);

    memcpy(frame + 0, mac, 6);                               // To our card
    static const uint8_t peer[6] = { 0x02, 0, 0, 0, 0, 0x01 };
    memcpy(frame + 6, peer, 6);
    frame[12] = 0x08; frame[13] = 0x00;                      // IPv4

    uint8_t *ip = frame + 14;
    uint16_t total = (uint16_t)(20 + 8 + payload_len);
    ip[0] = 0x45; ip[2] = (uint8_t)(total >> 8); ip[3] = (uint8_t)total;
    ip[8] = 64; ip[9] = 17;                                  // TTL, protocol UDP
    ip[12] = 10; ip[13] = 0; ip[14] = 2; ip[15] = 77;        // From 10.0.2.77
    ip[16] = 10; ip[17] = 0; ip[18] = 2; ip[19] = 15;        // To us
    uint16_t sum = net_checksum(ip, 20);
    ip[10] = (uint8_t)(sum >> 8); ip[11] = (uint8_t)sum;

    uint8_t *udp = ip + 20;
    udp[0] = 0x30; udp[1] = 0x39;                            // Source port 12345
    udp[2] = (uint8_t)(port >> 8); udp[3] = (uint8_t)port;
    udp[4] = (uint8_t)((8 + payload_len) >> 8); udp[5] = (uint8_t)(8 + payload_len);
    memcpy(udp + 8, payload, payload_len);                   // Checksum left 0: "not computed"
    return 14 + 20 + 8 + payload_len;
}

// Frames arrive from the outside world and may say anything. Each damaged
// frame here must be counted as dropped and must not reach the socket.
TEST(net_drops_malformed_frames) {
    static uint8_t frame[256], bad[256];
    char buf[32];
    udp_socket_t *sock = udp_open();
    EXPECT_EQ(udp_bind(sock, 7100), 0);

    // First, the intact frame is delivered
    size_t len = make_udp_frame(frame, 7100, "intact", 6);
    uint64_t delivered = net_stats()->udp_delivered;
    net_handle_frame(frame, len);
    EXPECT_TRUE(net_stats()->udp_delivered == delivered + 1);
    uint32_t from = 0;
    EXPECT_EQ(udp_recvfrom(sock, buf, sizeof(buf), &from, NULL, 5), 6);
    EXPECT_TRUE(from == NET_IP(10, 0, 2, 77));
    EXPECT_TRUE(net_arp_known(NET_IP(10, 0, 2, 77)));        // The sender's address was learned

    // A sender beyond the gateway is not learned (its frames carry the
    // gateway's address), and a crowd of them does not push the gateway's
    // own entry out of the cache, which replies to them need
    EXPECT_EQ(net_ping(NET_GATEWAY, 100, NULL), 0);
    for (uint8_t host = 1; host <= 20; host++) {
        size_t far_len = make_udp_frame(bad, 7100, "far", 3);
        uint8_t *ip = bad + 14;
        ip[12] = 192; ip[13] = 0; ip[14] = 2; ip[15] = host;   // 192.0.2.host: another network
        ip[10] = ip[11] = 0;
        uint16_t sum = net_checksum(ip, 20);
        ip[10] = (uint8_t)(sum >> 8); ip[11] = (uint8_t)sum;
        net_handle_frame(bad, far_len);
        EXPECT_EQ(udp_recvfrom(sock, buf, sizeof(buf), NULL, NULL, 5), 3);
    }
    EXPECT_TRUE(!net_arp_known(NET_IP(192, 0, 2, 1)));
    EXPECT_TRUE(net_arp_known(NET_GATEWAY));

#define EXPECT_DROPPED(setup, length) do { \
        memcpy(bad, frame, len); setup; \
        uint64_t dropped_before = net_stats()->dropped, delivered_before = net_stats()->udp_delivered; \
        net_handle_frame(bad, (length)); \
        EXPECT_TRUE(net_stats()->dropped == dropped_before + 1); \
        EXPECT_TRUE(net_stats()->udp_delivered == delivered_before); \
    } while (0)

    EXPECT_DROPPED((void)0, 10);                             // Shorter than an Ethernet header
    EXPECT_DROPPED((void)0, 14 + 10);                        // Cut off inside the IP header
    EXPECT_DROPPED((void)0, 14 + 20 + 4);                    // Cut off inside the UDP header
    EXPECT_DROPPED(bad[0] ^= 0x02, len);                     // Addressed to another card
    EXPECT_DROPPED(bad[13] = 0x99, len);                     // Unknown protocol in the Ethernet header
    EXPECT_DROPPED(bad[14] = 0x65, len);                     // IP version 6 in an IPv4 frame
    EXPECT_DROPPED(bad[14] = 0x41, len);                     // IP header length of 4 bytes
    EXPECT_DROPPED(bad[14] = 0x4F, len);                     // IP header length of 60: longer than the packet
    EXPECT_DROPPED(bad[17] = 0xFF, len);                     // IP total length beyond the frame
    EXPECT_DROPPED(bad[25] ^= 0x01, len);                    // Corrupted IP checksum
    EXPECT_DROPPED(bad[23] = 6, len);                        // (checksum now wrong: protocol changed)
    EXPECT_DROPPED(bad[14 + 20 + 5] = 0xFF, len);            // UDP length beyond the packet
    EXPECT_DROPPED(bad[14 + 20 + 5] = 3, len);               // UDP length smaller than its header
    EXPECT_DROPPED(bad[14 + 20 + 6] = 0x12, len);            // UDP checksum present and wrong
#undef EXPECT_DROPPED

    // A fragment (the "more fragments" flag set), with a correct IP checksum
    memcpy(bad, frame, len);
    bad[14 + 6] = 0x20; bad[14 + 10] = 0; bad[14 + 11] = 0;
    uint16_t sum = net_checksum(bad + 14, 20);
    bad[14 + 10] = (uint8_t)(sum >> 8); bad[14 + 11] = (uint8_t)sum;
    uint64_t dropped_before = net_stats()->dropped;
    net_handle_frame(bad, len);
    EXPECT_TRUE(net_stats()->dropped == dropped_before + 1);

    // ARP packets with wrong sizes or cut short
    static uint8_t arp[64];
    uint8_t mac[6];
    virtio_net_get_mac(mac);
    memset(arp, 0, sizeof(arp));
    memset(arp, 0xFF, 6);                                    // Broadcast
    arp[12] = 0x08; arp[13] = 0x06;
    arp[14 + 1] = 1; arp[14 + 2] = 0x08; arp[14 + 4] = 6; arp[14 + 5] = 4; arp[14 + 7] = 1;   // A request
    arp[14 + 24] = 10; arp[14 + 25] = 0; arp[14 + 26] = 2; arp[14 + 27] = 15;                 // "Who has 10.0.2.15?"
    uint64_t replies = net_stats()->arp_replies_sent;
    net_handle_frame(arp, 14 + 28);
    EXPECT_TRUE(net_stats()->arp_replies_sent == replies + 1);   // The well-formed one is answered
    dropped_before = net_stats()->dropped;
    net_handle_frame(arp, 14 + 20);                          // Truncated
    arp[14 + 4] = 8;                                         // Hardware address length 8
    net_handle_frame(arp, 14 + 28);
    EXPECT_TRUE(net_stats()->dropped == dropped_before + 2);
    EXPECT_TRUE(net_stats()->arp_replies_sent == replies + 1);

    // Nothing got through to the socket in all of that
    EXPECT_EQ(udp_recvfrom(sock, buf, sizeof(buf), NULL, NULL, 2), -ETIMEDOUT);
    udp_close(sock);

    // And the stack still works
    EXPECT_EQ(net_ping(NET_GATEWAY, 200, NULL), 0);
}

// The address is a setting, not a constant
TEST(net_configuration_can_be_changed) {
    net_config_t original, config;
    net_get_config(&original);
    EXPECT_TRUE(original.ip == NET_LOCAL_IP && original.netmask == NET_NETMASK);
    EXPECT_TRUE(original.gateway == NET_GATEWAY && original.dns == NET_DNS);
    uint8_t mac[6];
    virtio_net_get_mac(mac);
    EXPECT_EQ(memcmp(original.mac, mac, 6), 0);

    // Settings that could not work are refused, and nothing changes
    config = original; config.netmask = NET_IP(255, 0, 255, 0);            // A netmask with a hole
    EXPECT_EQ(net_set_config(&config), -EINVAL);
    config = original; config.netmask = 0;
    EXPECT_EQ(net_set_config(&config), -EINVAL);
    config = original; config.gateway = NET_IP(192, 168, 1, 1);            // Not on our network
    EXPECT_EQ(net_set_config(&config), -EINVAL);
    config = original; config.ip = NET_IP(127, 0, 0, 5);
    EXPECT_EQ(net_set_config(&config), -EINVAL);
    config = original; config.ip = NET_IP(10, 0, 2, 255);                  // The broadcast address
    EXPECT_EQ(net_set_config(&config), -EINVAL);
    config = original; config.ip = NET_IP(10, 0, 2, 0);                    // The network itself
    EXPECT_EQ(net_set_config(&config), -EINVAL);
    EXPECT_EQ(net_set_config(NULL), -EINVAL);
    net_get_config(&config);
    EXPECT_EQ(memcmp(&config, &original, sizeof(config)), 0);

    // Another address on the same network: we answer to it, the gateway
    // still answers us, and packets carry the new address as their source
    config = original;
    config.ip = NET_IP(10, 0, 2, 77);
    config.mac[0] ^= 0xFF;                                                 // Ignored: the card's address is what it is
    EXPECT_EQ(net_set_config(&config), 0);
    EXPECT_TRUE(net_source_for(NET_GATEWAY) == NET_IP(10, 0, 2, 77));
    EXPECT_TRUE(!net_arp_known(NET_GATEWAY));                              // Neighbours were forgotten
    EXPECT_EQ(net_ping(NET_IP(10, 0, 2, 77), 100, NULL), 0);
    EXPECT_EQ(net_ping(NET_GATEWAY, 200, NULL), 0);
    net_get_config(&config);
    EXPECT_EQ(memcmp(config.mac, mac, 6), 0);

    EXPECT_EQ(net_set_config(&original), 0);
    EXPECT_EQ(net_ping(NET_GATEWAY, 200, NULL), 0);
}

// A UDP datagram for port 6800 in an Ethernet frame for our card, from
// 10.0.2.2 to the IPv4 address `to`
static size_t build_udp_frame(uint8_t *frame, uint32_t to) {
    uint8_t mac[6];
    virtio_net_get_mac(mac);
    memset(frame, 0, 64);
    memcpy(frame, mac, 6);
    frame[6] = 0x52; frame[11] = 0x01;
    frame[12] = 0x08; frame[13] = 0x00;                                    // IPv4
    uint8_t *ip = frame + 14;
    ip[0] = 0x45; ip[3] = 20 + 8 + 4; ip[8] = 64; ip[9] = 17;              // 4 bytes of UDP payload
    ip[12] = 10; ip[13] = 0; ip[14] = 2; ip[15] = 2;
    ip[16] = (uint8_t)(to >> 24); ip[17] = (uint8_t)(to >> 16); ip[18] = (uint8_t)(to >> 8); ip[19] = (uint8_t)to;
    uint16_t sum = net_checksum(ip, 20);
    ip[10] = (uint8_t)(sum >> 8); ip[11] = (uint8_t)sum;
    uint8_t *udp = ip + 20;
    udp[0] = 0x1A; udp[1] = 0x90; udp[2] = 0x1A; udp[3] = 0x90;            // Ports 6800
    udp[5] = 12;                                                           // Length; checksum 0 = none
    memcpy(udp + 8, "dhcp", 4);
    return 14 + 20 + 12;
}

// Without an address the machine can broadcast and can be answered at any
// address (which is how DHCP hands one out), and nothing else
TEST(net_unconfigured_broadcasts_and_listens) {
    net_config_t original, none;
    net_get_config(&original);
    memset(&none, 0, sizeof(none));

    udp_socket_t *sock = udp_open();
    EXPECT_TRUE(sock != NULL);
    EXPECT_EQ(udp_bind(sock, 6800), 0);
    uint8_t frame[64], buf[16];
    size_t len;

    // Configured: a datagram for someone else's address is not ours, one to
    // the broadcast address is
    len = build_udp_frame(frame, NET_IP(10, 0, 2, 99));
    net_handle_frame(frame, len);
    EXPECT_EQ(udp_recvfrom(sock, buf, sizeof(buf), NULL, NULL, 2), -ETIMEDOUT);
    len = build_udp_frame(frame, NET_BROADCAST);
    net_handle_frame(frame, len);
    EXPECT_EQ(udp_recvfrom(sock, buf, sizeof(buf), NULL, NULL, 2), 4);
    len = build_udp_frame(frame, NET_IP(10, 0, 2, 255));                   // Our network's broadcast address
    net_handle_frame(frame, len);
    EXPECT_EQ(udp_recvfrom(sock, buf, sizeof(buf), NULL, NULL, 2), 4);

    EXPECT_EQ(net_set_config(&none), 0);
    EXPECT_TRUE(net_source_for(NET_BROADCAST) == 0);

    // Unconfigured: the same datagram for "someone else" is taken
    len = build_udp_frame(frame, NET_IP(10, 0, 2, 99));
    net_handle_frame(frame, len);
    EXPECT_EQ(udp_recvfrom(sock, buf, sizeof(buf), NULL, NULL, 2), 4);
    EXPECT_EQ(memcmp(buf, "dhcp", 4), 0);

    // Only broadcasts can be sent
    EXPECT_EQ(udp_sendto(sock, NET_GATEWAY, 6800, "x", 1), -ENETDOWN);
    EXPECT_EQ(net_ping(NET_GATEWAY, 20, NULL), -ENETDOWN);
    EXPECT_EQ(udp_sendto(sock, NET_BROADCAST, 6800, "x", 1), 1);

    udp_close(sock);
    EXPECT_EQ(net_set_config(&original), 0);
    EXPECT_EQ(net_ping(NET_GATEWAY, 200, NULL), 0);
}

void test_suite_net(void) {
    kprintf("Suite: Network (virtio-net, ARP, IPv4, ICMP, UDP)\n");
    RUN_TEST(pci_finds_the_devices_qemu_provides);
    RUN_TEST(net_checksum_known_values);
    RUN_TEST(net_card_and_stack_are_up);
    RUN_TEST(net_ping_gateway);
    RUN_TEST(net_arp_answers_age);
    RUN_TEST(net_ping_ourselves_and_nobody);
    RUN_TEST(udp_sockets_over_loopback);
    RUN_TEST(net_drops_malformed_frames);
    RUN_TEST(net_configuration_can_be_changed);
    RUN_TEST(net_unconfigured_broadcasts_and_listens);
}
