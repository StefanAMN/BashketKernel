/* udpecho: a UDP echo server. Every datagram received on PORT is sent back
 * to where it came from.
 *
 *   udpecho PORT [COUNT]      stop after COUNT datagrams (default: never)
 */
#include "ulib.h"

int main(int argc, char **argv) {
    long port = argc > 1 ? atoi(argv[1]) : 0;
    long limit = argc > 2 ? atoi(argv[2]) : -1;
    if (port <= 0 || port > 65535) {
        fprintf(2, "usage: udpecho PORT [COUNT]\n");
        return 2;
    }

    long fd = socket();
    long err = fd < 0 ? fd : bind((int)fd, (unsigned)port);
    if (err < 0) {
        fprintf(2, "udpecho: %s\n", strerror(err));
        return 1;
    }
    printf("udpecho: listening on port %ld\n", port);

    static char data[1472];
    for (long handled = 0; limit < 0 || handled < limit; handled++) {
        struct net_address from;
        long n = recvfrom((int)fd, data, sizeof(data), &from, 0);
        if (n == -EINTR) { handled--; continue; }
        if (n < 0) {
            fprintf(2, "udpecho: receive: %s\n", strerror(n));
            return 1;
        }

        char address[16];
        format_ip(from.ip, address);
        printf("udpecho: %ld bytes from %s port %u\n", n, address, (unsigned)from.port);

        long sent = sendto((int)fd, data, (size_t)n, from.ip, from.port);
        if (sent < 0) fprintf(2, "udpecho: reply: %s\n", strerror(sent));
    }
    return 0;
}
