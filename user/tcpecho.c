/* tcpecho: a TCP server that sends back whatever it receives.
 *
 *   tcpecho PORT [COUNT]     serve COUNT connections, one after another, then
 *                            exit (default: for ever)
 */
#include "ulib.h"

int main(int argc, char **argv) {
    long port = argc > 1 ? atoi(argv[1]) : 0;
    long count = argc > 2 ? atoi(argv[2]) : -1;
    if (argc < 2 || argc > 3 || port <= 0 || port > 65535) {
        fprintf(2, "usage: tcpecho PORT [COUNT]\n");
        return 2;
    }

    long server = socket(SOCK_STREAM);
    long err = server < 0 ? server : bind((int)server, (unsigned)port);
    if (err == 0) err = listen((int)server, 4);
    if (err < 0) {
        fprintf(2, "tcpecho: %s\n", strerror(err));
        return 1;
    }
    printf("tcpecho: listening on port %u\n", (unsigned)port);

    for (long served = 0; count < 0 || served < count; served++) {
        struct net_address from;
        long conn = accept((int)server, &from);
        if (conn < 0) {
            fprintf(2, "tcpecho: accept: %s\n", strerror(conn));
            return 1;
        }

        static char buf[4096];
        unsigned long total = 0;
        long n;
        while ((n = read((int)conn, buf, sizeof(buf))) > 0) {
            long sent = 0;
            while (sent < n) {
                long w = write((int)conn, buf + sent, (size_t)(n - sent));
                if (w <= 0) { n = w; break; }
                sent += w;
            }
            if (n <= 0) break;
            total += (unsigned long)n;
        }
        close((int)conn);

        char address[16];
        format_ip(from.ip, address);
        printf("tcpecho: %lu bytes echoed to %s port %u%s\n", total, address, (unsigned)from.port,
               n < 0 ? " (ended by an error)" : "");
    }
    return 0;
}
