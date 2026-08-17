/* dhcp: ask the network for an address (RFC 2131) and configure the
 * interface with the answer.
 *
 *   dhcp          ask, configure, and print the result
 *   dhcp show     only print the current settings
 *
 * The exchange is four datagrams, all broadcast, because we have no address
 * to be answered at:
 *
 *   us -> everyone   DISCOVER  "is there a server?"
 *   server -> us     OFFER     "you could have 10.0.2.15"
 *   us -> everyone   REQUEST   "I take 10.0.2.15 from that server"
 *   server -> us     ACK       "it is yours", with netmask, gateway and DNS
 */
#include "ulib.h"

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68
#define BROADCAST 0xFFFFFFFFu

#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5
#define DHCP_NAK      6

#define OPT_NETMASK   1
#define OPT_ROUTER    3
#define OPT_DNS       6
#define OPT_REQUESTED 50
#define OPT_LEASE     51
#define OPT_TYPE      53
#define OPT_SERVER    54
#define OPT_WANTED    55
#define OPT_END       255

#define FIXED_PART 236              /* Up to and including the boot file name */
#define OPTIONS_AT (FIXED_PART + 4) /* After the magic number that says "options follow" */

/* Big-endian 32-bit fields of a message: write and read */
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

/* (See put32) */
static uint32_t get32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* What a server's message said */
struct lease {
    int type;
    uint32_t ip, netmask, gateway, dns, server, seconds;
};

/* Build a DHCP message of `type` (DISCOVER, or REQUEST for `requested`
 * from `server`) for our card `mac`, exchange `xid`, in the 548 bytes at
 * `msg`. Returns its length. */
static size_t build(uint8_t *msg, int type, uint32_t xid, const uint8_t mac[6], uint32_t requested, uint32_t server) {
    memset(msg, 0, 548);
    msg[0] = 1;                     /* A request (2 is a reply) */
    msg[1] = 1;                     /* Ethernet... */
    msg[2] = 6;                     /* ...whose addresses are six bytes */
    put32(msg + 4, xid);            /* Our name for this exchange: replies repeat it */
    msg[10] = 0x80;                 /* Flags: answer by broadcast, we cannot be addressed yet */
    memcpy(msg + 28, mac, 6);       /* Who is asking */
    put32(msg + FIXED_PART, 0x63825363);

    size_t pos = OPTIONS_AT;
    msg[pos++] = OPT_TYPE; msg[pos++] = 1; msg[pos++] = (uint8_t)type;
    if (requested) { msg[pos++] = OPT_REQUESTED; msg[pos++] = 4; put32(msg + pos, requested); pos += 4; }
    if (server) { msg[pos++] = OPT_SERVER; msg[pos++] = 4; put32(msg + pos, server); pos += 4; }
    msg[pos++] = OPT_WANTED; msg[pos++] = 3; msg[pos++] = OPT_NETMASK; msg[pos++] = OPT_ROUTER; msg[pos++] = OPT_DNS;
    msg[pos++] = OPT_END;
    return pos < 300 ? 300 : pos;   /* Some servers drop messages shorter than BOOTP's 300 bytes */
}

/* Returns false for anything that is not a well-formed reply to us */
static bool parse(const uint8_t *msg, size_t len, uint32_t xid, const uint8_t mac[6], struct lease *out) {
    memset(out, 0, sizeof(*out));
    if (len < OPTIONS_AT + 1 || msg[0] != 2) return false;
    if (get32(msg + 4) != xid || memcmp(msg + 28, mac, 6) != 0) return false;
    if (get32(msg + FIXED_PART) != 0x63825363) return false;
    out->ip = get32(msg + 16);      /* "Your address" */

    /* Options: a type byte, a length byte, then that many bytes of value */
    for (size_t pos = OPTIONS_AT; pos < len; ) {
        uint8_t option = msg[pos++];
        if (option == OPT_END) break;
        if (option == 0) continue;  /* Padding */
        if (pos >= len) return false;
        size_t option_len = msg[pos++];
        if (pos + option_len > len) return false;

        if (option == OPT_TYPE && option_len == 1) out->type = msg[pos];
        else if (option == OPT_NETMASK && option_len == 4) out->netmask = get32(msg + pos);
        else if (option == OPT_ROUTER && option_len >= 4) out->gateway = get32(msg + pos); /* The first of them */
        else if (option == OPT_DNS && option_len >= 4) out->dns = get32(msg + pos);
        else if (option == OPT_SERVER && option_len == 4) out->server = get32(msg + pos);
        else if (option == OPT_LEASE && option_len == 4) out->seconds = get32(msg + pos);
        pos += option_len;
    }
    return out->type != 0;
}

/* Send `msg` and wait for a reply of type `want` (or a refusal) */
static bool exchange(int sock, const uint8_t *msg, size_t len, uint32_t xid, const uint8_t mac[6],
                     int want, struct lease *reply) {
    static uint8_t in[1472];
    for (int attempt = 0; attempt < 4; attempt++) {
        if (sendto(sock, msg, len, BROADCAST, DHCP_SERVER_PORT) < 0) return false;

        unsigned long deadline = clock_ns() + 1000000000UL;
        while (clock_ns() < deadline) {
            unsigned long left_ms = (deadline - clock_ns()) / 1000000UL + 1;
            long n = recvfrom(sock, in, sizeof(in), NULL, left_ms);
            if (n < 0) break;
            if (!parse(in, (size_t)n, xid, mac, reply)) continue; /* Someone else's, or noise */
            if (reply->type == want || reply->type == DHCP_NAK) return true;
        }
    }
    return false;
}

/* Print a configuration, labelled `what` */
static void show(const char *what, const struct net_config *c) {
    char ip[16], mask[16], gateway[16], dns[16];
    format_ip(c->ip, ip); format_ip(c->netmask, mask); format_ip(c->gateway, gateway); format_ip(c->dns, dns);
    if (c->ip == 0) printf("%s: not configured\n", what);
    else printf("%s: address %s netmask %s gateway %s dns %s\n", what, ip, mask, gateway, dns);
}

int main(int argc, char **argv) {
    struct net_config old;
    long err = netconfig(&old, NULL);
    if (err < 0) {
        fprintf(2, "dhcp: %s\n", strerror(err));
        return 1;
    }
    if (argc == 2 && strcmp(argv[1], "show") == 0) {
        show("interface", &old);
        return 0;
    }
    if (argc != 1) {
        fprintf(2, "usage: dhcp [show]\n");
        return 2;
    }

    long sock = socket(SOCK_DGRAM);
    err = sock < 0 ? sock : bind((int)sock, DHCP_CLIENT_PORT);
    if (err < 0) {
        fprintf(2, "dhcp: %s\n", strerror(err));
        return 1;
    }

    /* Give up the address we have: from here until the server answers, the
     * machine can only broadcast */
    struct net_config none;
    memset(&none, 0, sizeof(none));
    err = netconfig(NULL, &none);
    if (err < 0) {
        fprintf(2, "dhcp: cannot change the interface: %s\n", strerror(err));
        return 1;
    }

    uint32_t xid = (uint32_t)clock_ns() ^ ((uint32_t)old.mac[4] << 8) ^ old.mac[5];
    long fd = open("/dev/random", O_RDONLY);
    if (fd >= 0) {
        read((int)fd, &xid, sizeof(xid));
        close((int)fd);
    }

    static uint8_t msg[548];
    struct lease offer, ack;
    bool ok = exchange((int)sock, msg, build(msg, DHCP_DISCOVER, xid, old.mac, 0, 0), xid, old.mac, DHCP_OFFER, &offer) &&
              offer.type == DHCP_OFFER && offer.ip != 0;
    if (ok) {
        ok = exchange((int)sock, msg, build(msg, DHCP_REQUEST, xid, old.mac, offer.ip, offer.server), xid, old.mac, DHCP_ACK, &ack) &&
             ack.type == DHCP_ACK && ack.ip != 0;
    }
    close((int)sock);

    if (ok) {
        struct net_config new;
        memset(&new, 0, sizeof(new));
        new.ip = ack.ip;
        new.netmask = ack.netmask ? ack.netmask : 0xFFFFFF00u; /* A server that names no netmask: assume /24 */
        new.gateway = ack.gateway;
        new.dns = ack.dns;
        err = netconfig(NULL, &new);
        if (err == 0) {
            show("dhcp", &new);
            if (ack.seconds) printf("dhcp: lease of %u seconds (not renewed: run dhcp again)\n", (unsigned)ack.seconds);
            return 0;
        }
        fprintf(2, "dhcp: the server's answer can not be used: %s\n", strerror(err));
    } else {
        fprintf(2, "dhcp: no answer from a server\n");
    }

    /* Leave the machine as it was rather than without a network */
    netconfig(NULL, &old);
    show("dhcp: keeping", &old);
    return 1;
}
