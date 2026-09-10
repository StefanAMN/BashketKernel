/* dhcp: ask the network for an address (RFC 2131) and configure the
 * interface with the answer.
 *
 *   dhcp                         ask, configure, print the result and exit
 *   dhcp -d [-t SECONDS] [-n N]  the same, then stay and keep the lease
 *   dhcp show                    only print the current settings
 *   dhcp -T                      check the lease arithmetic and exit (0: right)
 *
 * The exchange is four datagrams, all broadcast, because we have no address
 * to be answered at:
 *
 *   us -> everyone   DISCOVER  "is there a server?"
 *   server -> us     OFFER     "you could have 10.0.2.15"
 *   us -> everyone   REQUEST   "I take 10.0.2.15 from that server"
 *   server -> us     ACK       "it is yours", with netmask, gateway and DNS
 *
 * A lease runs out. With -d the program stays and does what RFC 2131
 * section 4.4.5 says: at time T1 (half the lease, unless the server named
 * another) it asks the server that gave the lease, by a REQUEST sent to that
 * server alone, for more time ("renewing"); if that server has not answered
 * by T2 (7/8 of the lease) it asks any server, by broadcast ("rebinding");
 * when the lease ends it gives the address up and starts again from
 * DISCOVER. A refusal (NAK) also starts again. -t SECONDS puts T1 that far
 * after each lease is granted, and -n N exits after N renewals: both are for
 * tests, which cannot wait half a day.
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
#define OPT_T1        58
#define OPT_T2        59
#define OPT_END       255

#define FIXED_PART 236              /* Up to and including the boot file name */
#define OPTIONS_AT (FIXED_PART + 4) /* After the magic number that says "options follow" */

#define LEASE_INFINITE 0xFFFFFFFFu  /* RFC 2132 9.2: a lease that does not end */
#define LEASE_MIN      60           /* Shorter leases (0 included) are taken as this long */
#define RETRY_MIN      60           /* RFC 2131 4.4.5: wait at least this long between retransmissions */

/* Big-endian 32-bit fields of a message: write and read */
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

/* (See put32) */
static uint32_t get32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* What a server's message said (t1 and t2 are 0 when it did not say) */
struct lease {
    int type;
    uint32_t ip, netmask, gateway, dns, server, seconds, t1, t2;
};

/* When, in seconds after a lease was granted, to renew, to rebind and to
 * give it up. Always t1 <= t2 <= end; meaningless if `infinite`. */
struct lease_times {
    uint32_t t1, t2, end;
    bool infinite;
};

/* Work out the times for a lease of `lease` seconds whose server suggested
 * `t1` and `t2` (0: did not). The suggestions are used only if they make
 * sense (0 < T1 < T2 < lease); otherwise RFC 2131's defaults, 1/2 and 7/8
 * of the lease. A lease shorter than LEASE_MIN, 0 included, counts as
 * LEASE_MIN, so that a hostile or broken server cannot make us renew in a
 * tight loop. `override_t1` (0: none) replaces T1, as -t does; T2 then
 * moves halfway between it and the end if it no longer comes after it. */
static void lease_timing(uint32_t lease, uint32_t t1, uint32_t t2, uint32_t override_t1, struct lease_times *out) {
    memset(out, 0, sizeof(*out));
    if (lease == LEASE_INFINITE) {
        out->infinite = true;
        return;
    }
    if (lease < LEASE_MIN) lease = LEASE_MIN;
    out->end = lease;
    if (t1 > 0 && t2 > t1 && t2 < lease) {
        out->t1 = t1;
        out->t2 = t2;
    } else {
        out->t1 = lease / 2;
        out->t2 = (uint32_t)((uint64_t)lease * 7 / 8);    /* (64 bits: the product can exceed 2^32) */
        /* A T1 alone can still be used if it comes before the default T2 */
        if (t1 > 0 && t1 < out->t2 && t2 == 0) out->t1 = t1;
    }
    if (override_t1) {
        out->t1 = override_t1 < lease ? override_t1 : lease / 2;
        if (out->t2 <= out->t1) out->t2 = out->t1 + (lease - out->t1 + 1) / 2;
    }
}

enum { STEP_WAIT, STEP_RENEW, STEP_REBIND, STEP_EXPIRED };

/* What to do `elapsed` seconds into a lease with times `t`: wait (bound),
 * ask the server (renewing), ask anyone (rebinding), or give the address
 * up. For the first three, *wait is how long to wait before looking again:
 * until T1; or, after a request, half the time left in this phase but at
 * least RETRY_MIN seconds (RFC 2131 4.4.5), and never past the phase's
 * end, so that the next phase starts on time. */
static int lease_step(const struct lease_times *t, uint32_t elapsed, uint32_t *wait) {
    *wait = 0;
    if (elapsed >= t->end) return STEP_EXPIRED;
    if (elapsed < t->t1) {
        *wait = t->t1 - elapsed;
        return STEP_WAIT;
    }
    int step = elapsed < t->t2 ? STEP_RENEW : STEP_REBIND;
    uint32_t left = (step == STEP_RENEW ? t->t2 : t->end) - elapsed;
    uint32_t retry = left / 2 > RETRY_MIN ? left / 2 : RETRY_MIN;
    *wait = retry < left ? retry : left;
    return step;
}

/* Build a DHCP message of `type` (DISCOVER, or REQUEST for `requested`
 * from `server`) for our card `mac`, exchange `xid`, in the 548 bytes at
 * `msg`. `ciaddr` is the address we already hold (renewing and rebinding:
 * then `requested` and `server` are 0, and the answer may come to that
 * address, so the broadcast flag stays off). Returns its length. */
static size_t build(uint8_t *msg, int type, uint32_t xid, const uint8_t mac[6], uint32_t requested,
                    uint32_t server, uint32_t ciaddr) {
    memset(msg, 0, 548);
    msg[0] = 1;                     /* A request (2 is a reply) */
    msg[1] = 1;                     /* Ethernet... */
    msg[2] = 6;                     /* ...whose addresses are six bytes */
    put32(msg + 4, xid);            /* Our name for this exchange: replies repeat it */
    if (!ciaddr) msg[10] = 0x80;    /* Flags: answer by broadcast, we cannot be addressed yet */
    put32(msg + 12, ciaddr);        /* "Client address": ours, if we have one */
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
        else if (option == OPT_T1 && option_len == 4) out->t1 = get32(msg + pos);
        else if (option == OPT_T2 && option_len == 4) out->t2 = get32(msg + pos);
        pos += option_len;
    }
    return out->type != 0;
}

/* Wait up to `timeout_ms` for a reply to exchange `xid` of type `want` (or
 * a refusal) on `sock`. True with it in *reply. */
static bool await_reply(int sock, uint32_t xid, const uint8_t mac[6], int want, unsigned long timeout_ms,
                        struct lease *reply) {
    static uint8_t in[1472];
    unsigned long deadline = clock_ns() + timeout_ms * 1000000UL;
    while (clock_ns() < deadline) {
        unsigned long left_ms = (deadline - clock_ns()) / 1000000UL + 1;
        long n = recvfrom(sock, in, sizeof(in), NULL, left_ms);
        if (n < 0) break;
        if (!parse(in, (size_t)n, xid, mac, reply)) continue; /* Someone else's, or noise */
        if (reply->type == want || reply->type == DHCP_NAK) return true;
    }
    return false;
}

/* Broadcast `msg` and wait for a reply of type `want` (or a refusal): four
 * tries, a second each */
static bool exchange(int sock, const uint8_t *msg, size_t len, uint32_t xid, const uint8_t mac[6],
                     int want, struct lease *reply) {
    for (int attempt = 0; attempt < 4; attempt++) {
        if (sendto(sock, msg, len, BROADCAST, DHCP_SERVER_PORT) < 0) return false;
        if (await_reply(sock, xid, mac, want, 1000, reply)) return true;
    }
    return false;
}

/* A fresh exchange number: from /dev/random, else from the clock and `mac` */
static uint32_t new_xid(const uint8_t mac[6]) {
    uint32_t xid = (uint32_t)clock_ns() ^ ((uint32_t)mac[4] << 8) ^ mac[5];
    long fd = open("/dev/random", O_RDONLY);
    if (fd >= 0) {
        read((int)fd, &xid, sizeof(xid));
        close((int)fd);
    }
    return xid;
}

/* DISCOVER, then REQUEST what was offered. True with the server's ACK in
 * *ack. The interface must already have no address (we are broadcasting). */
static bool acquire(int sock, const uint8_t mac[6], struct lease *ack) {
    static uint8_t msg[548];
    struct lease offer;
    uint32_t xid = new_xid(mac);
    if (!exchange(sock, msg, build(msg, DHCP_DISCOVER, xid, mac, 0, 0, 0), xid, mac, DHCP_OFFER, &offer) ||
        offer.type != DHCP_OFFER || offer.ip == 0) {
        return false;
    }
    return exchange(sock, msg, build(msg, DHCP_REQUEST, xid, mac, offer.ip, offer.server, 0), xid, mac, DHCP_ACK, ack) &&
           ack->type == DHCP_ACK && ack->ip != 0;
}

/* Configure the interface from an ACK. 0, or a negative error. */
static long apply(const struct lease *ack) {
    struct net_config new;
    memset(&new, 0, sizeof(new));
    new.ip = ack->ip;
    new.netmask = ack->netmask ? ack->netmask : 0xFFFFFF00u; /* A server that names no netmask: assume /24 */
    new.gateway = ack->gateway;
    new.dns = ack->dns;
    return netconfig(NULL, &new);
}

/* Take the interface's address away, so that only broadcasts work */
static void unconfigure(void) {
    struct net_config none;
    memset(&none, 0, sizeof(none));
    netconfig(NULL, &none);
}

/* Print a configuration, labelled `what` */
static void show(const char *what, const struct net_config *c) {
    char ip[16], mask[16], gateway[16], dns[16];
    format_ip(c->ip, ip); format_ip(c->netmask, mask); format_ip(c->gateway, gateway); format_ip(c->dns, dns);
    if (c->ip == 0) printf("%s: not configured\n", what);
    else printf("%s: address %s netmask %s gateway %s dns %s\n", what, ip, mask, gateway, dns);
}

/* Seconds since `start` (a clock_ns() value), rounded down */
static uint32_t seconds_since(unsigned long start) {
    return (uint32_t)((clock_ns() - start) / 1000000000UL);
}

/* Keep the lease `ack` (just applied) for ever, or until `renewals_wanted`
 * renewals (0: no limit) have succeeded. `override_t1` is -t's value (0:
 * none). Returns the exit status. */
static int keep_lease(int sock, const uint8_t mac[6], struct lease ack, uint32_t override_t1, long renewals_wanted) {
    static uint8_t msg[548];
    long renewals = 0;
    for (;;) {
        struct lease_times times;
        lease_timing(ack.seconds, ack.t1, ack.t2, override_t1, &times);
        if (times.infinite) {
            printf("dhcp: the lease does not end; nothing to renew\n");
            return 0;
        }
        unsigned long granted = clock_ns();
        bool restart = false;
        while (!restart) {
            uint32_t wait;
            int step = lease_step(&times, seconds_since(granted), &wait);
            if (step == STEP_WAIT) {
                sleep_ms((unsigned long)wait * 1000);
                continue;
            }
            if (step == STEP_EXPIRED) {
                printf("dhcp: the lease ran out; asking again\n");
                restart = true;
                break;
            }

            /* Renewing goes to the server that gave the lease (if it said
             * who it is); rebinding to anyone. Both name the address we
             * hold, and the reply comes to that address. */
            uint32_t xid = new_xid(mac);
            uint32_t to = step == STEP_RENEW && ack.server ? ack.server : BROADCAST;
            struct lease reply;
            size_t len = build(msg, DHCP_REQUEST, xid, mac, 0, 0, ack.ip);
            if (sendto(sock, msg, len, to, DHCP_SERVER_PORT) >= 0 &&
                await_reply(sock, xid, mac, DHCP_ACK, (unsigned long)wait * 1000, &reply)) {
                if (reply.type == DHCP_NAK || reply.ip != ack.ip) {
                    printf("dhcp: the server refused the renewal; asking again\n");
                    restart = true;
                    break;
                }
                if (!reply.server) reply.server = ack.server;
                ack = reply;
                if (apply(&ack) < 0) {
                    restart = true;
                    break;
                }
                char server[16];
                format_ip(to, server);
                printf("dhcp: renewed by %s (%s), lease of %u seconds\n",
                       step == STEP_RENEW ? "the server" : "broadcast", server, (unsigned)ack.seconds);
                if (renewals_wanted && ++renewals >= renewals_wanted) return 0;
                break;                          /* A new lease: times from now */
            }
            /* No answer within `wait`: look at the clock again */
        }
        if (!restart) continue;

        /* Start over: no address until a server gives one */
        unconfigure();
        while (!acquire(sock, mac, &ack) || apply(&ack) < 0) sleep_ms(10000);
        struct net_config now;
        if (netconfig(&now, NULL) == 0) show("dhcp", &now);
    }
}

/* -T: the lease arithmetic against cases worked out by hand. Prints each
 * wrong one; returns the exit status (0: all right). */
static int self_test(void) {
    int failed = 0;
    struct lease_times t;
    uint32_t wait;
#define EXPECT(cond) do { if (!(cond)) { printf("dhcp -T: FAIL line %d: %s\n", __LINE__, #cond); failed++; } } while (0)

    lease_timing(86400, 0, 0, 0, &t);                    /* QEMU's lease: a day, no T1 or T2 */
    EXPECT(!t.infinite && t.t1 == 43200 && t.t2 == 75600 && t.end == 86400);
    lease_timing(1000, 300, 600, 0, &t);                 /* The server's own times */
    EXPECT(t.t1 == 300 && t.t2 == 600 && t.end == 1000);
    lease_timing(1000, 300, 0, 0, &t);                   /* Only T1: kept, T2 the default */
    EXPECT(t.t1 == 300 && t.t2 == 875);
    lease_timing(1000, 2000, 3000, 0, &t);               /* Past the end: the defaults */
    EXPECT(t.t1 == 500 && t.t2 == 875);
    lease_timing(1000, 800, 700, 0, &t);                 /* T1 after T2: the defaults */
    EXPECT(t.t1 == 500 && t.t2 == 875);
    lease_timing(1000, 900, 0, 0, &t);                   /* T1 after the default T2 */
    EXPECT(t.t1 == 500 && t.t2 == 875);
    lease_timing(0, 0, 0, 0, &t);                        /* No lease at all: the minimum */
    EXPECT(!t.infinite && t.end == LEASE_MIN && t.t1 == 30 && t.t2 == 52);
    lease_timing(LEASE_INFINITE, 10, 20, 1, &t);         /* For ever: never renewed, -t or not */
    EXPECT(t.infinite);
    lease_timing(0xFFFFFFFEu, 0, 0, 0, &t);              /* No overflow on the longest finite lease */
    EXPECT(t.t1 == 0x7FFFFFFFu && t.t2 == 0xDFFFFFFEu && t.end == 0xFFFFFFFEu);
    lease_timing(86400, 0, 0, 1, &t);                    /* -t 1 */
    EXPECT(t.t1 == 1 && t.t2 == 75600);
    lease_timing(86400, 0, 0, 100000, &t);               /* -t past the end: ignored */
    EXPECT(t.t1 == 43200 && t.t2 == 75600);
    lease_timing(100, 0, 0, 95, &t);                     /* -t after T2: T2 moves on */
    EXPECT(t.t1 == 95 && t.t2 == 98 && t.end == 100);
    lease_timing(100, 0, 0, 99, &t);                     /* ... and never past the end */
    EXPECT(t.t1 == 99 && t.t2 == 100 && t.end == 100);

    lease_timing(1000, 0, 0, 0, &t);                     /* 500, 875, 1000 */
    EXPECT(lease_step(&t, 0, &wait) == STEP_WAIT && wait == 500);
    EXPECT(lease_step(&t, 499, &wait) == STEP_WAIT && wait == 1);
    EXPECT(lease_step(&t, 500, &wait) == STEP_RENEW && wait == 187);    /* Half of 375 */
    EXPECT(lease_step(&t, 800, &wait) == STEP_RENEW && wait == 60);     /* 75 left: the minimum */
    EXPECT(lease_step(&t, 850, &wait) == STEP_RENEW && wait == 25);     /* ... but not past T2 */
    EXPECT(lease_step(&t, 875, &wait) == STEP_REBIND && wait == 62);    /* Half of 125 */
    EXPECT(lease_step(&t, 990, &wait) == STEP_REBIND && wait == 10);
    EXPECT(lease_step(&t, 1000, &wait) == STEP_EXPIRED);
    EXPECT(lease_step(&t, 0xFFFFFFFFu, &wait) == STEP_EXPIRED);
    lease_timing(100, 0, 0, 99, &t);                     /* T2 at the end: no rebinding */
    EXPECT(lease_step(&t, 99, &wait) == STEP_RENEW && wait == 1);
    EXPECT(lease_step(&t, 100, &wait) == STEP_EXPIRED);
#undef EXPECT

    if (failed) return 1;
    printf("dhcp -T: lease times right\n");
    return 0;
}

/* A whole decimal number from `s`, or -1 if it is not one */
static long number(const char *s) {
    long n = 0;
    if (!s || !*s) return -1;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || n > 100000000) return -1;
        n = n * 10 + (*s - '0');
    }
    return n;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "-T") == 0) return self_test();  /* Needs no network */

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
    bool stay = false;
    long override_t1 = 0, renewals_wanted = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0) {
            stay = true;
        } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc && (override_t1 = number(argv[i + 1])) > 0) {
            i++;
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc && (renewals_wanted = number(argv[i + 1])) > 0) {
            i++;
        } else {
            fprintf(2, "usage: dhcp [-d [-t SECONDS] [-n RENEWALS]] | dhcp show | dhcp -T\n");
            return 2;
        }
    }
    if (!stay && (override_t1 || renewals_wanted)) {
        fprintf(2, "dhcp: -t and -n only mean something with -d\n");
        return 2;
    }

    long sock = socket(SOCK_DGRAM);
    err = sock < 0 ? sock : bind((int)sock, DHCP_CLIENT_PORT);
    if (err < 0) {
        fprintf(2, "dhcp: %s\n", strerror(err));
        if (sock >= 0) close((int)sock);
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

    struct lease ack;
    if (acquire((int)sock, old.mac, &ack)) {
        err = apply(&ack);
        if (err == 0) {
            struct net_config now;
            netconfig(&now, NULL);
            show("dhcp", &now);
            if (stay) return keep_lease((int)sock, old.mac, ack, (uint32_t)override_t1, renewals_wanted);
            close((int)sock);
            if (ack.seconds) printf("dhcp: lease of %u seconds (not renewed: run dhcp -d to keep it)\n", (unsigned)ack.seconds);
            return 0;
        }
        fprintf(2, "dhcp: the server's answer can not be used: %s\n", strerror(err));
    } else {
        fprintf(2, "dhcp: no answer from a server\n");
    }
    close((int)sock);

    /* Leave the machine as it was rather than without a network */
    netconfig(NULL, &old);
    show("dhcp: keeping", &old);
    return 1;
}
