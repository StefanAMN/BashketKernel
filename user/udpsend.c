/* udpsend: send one UDP datagram and print the answer, if one comes.
 *
 *   udpsend ADDRESS PORT TEXT      for example: udpsend 10.0.2.2 5000 hello
 */
#include "ulib.h"

int main(int argc, char **argv) {
    uint32_t ip = argc > 1 ? parse_ip(argv[1]) : 0;
    long port = argc > 2 ? atoi(argv[2]) : 0;
    if (argc != 4 || ip == 0 || port <= 0 || port > 65535) {
        fprintf(2, "usage: udpsend ADDRESS PORT TEXT\n");
        return 2;
    }

    long fd = socket();
    if (fd < 0) {
        fprintf(2, "udpsend: %s\n", strerror(fd));
        return 1;
    }

    long sent = sendto((int)fd, argv[3], strlen(argv[3]), ip, (unsigned)port);
    if (sent < 0) {
        fprintf(2, "udpsend: %s\n", strerror(sent));
        return 1;
    }

    static char reply[1473];
    struct net_address from;
    long n = recvfrom((int)fd, reply, sizeof(reply) - 1, &from, 2000);
    if (n < 0) {
        printf("udpsend: no answer: %s\n", strerror(n));
        return 1;
    }
    reply[n] = '\0';

    char address[16];
    format_ip(from.ip, address);
    printf("answer from %s port %u: %s\n", address, (unsigned)from.port, reply);
    return 0;
}
