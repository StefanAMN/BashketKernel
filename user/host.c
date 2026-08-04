/* host: look a name up in the DNS and print its address.
 *
 *   host NAME                  ask the configured server (see dhcp)
 *   host NAME SERVER [PORT]    ask this one instead
 */
#include "ulib.h"

int main(int argc, char **argv) {
    if (argc < 2 || argc > 4) {
        fprintf(2, "usage: host NAME [SERVER [PORT]]\n");
        return 2;
    }

    uint32_t ip = 0;
    long err;
    if (argc == 2) {
        err = resolve(argv[1], &ip);
    } else {
        uint32_t server = parse_ip(argv[2]);
        long port = argc == 4 ? atoi(argv[3]) : 53;
        if (server == 0 || port <= 0 || port > 65535) {
            fprintf(2, "host: SERVER is an address like 10.0.2.3, PORT a number\n");
            return 2;
        }
        err = dns_lookup(server, (unsigned)port, argv[1], &ip);
    }

    if (err == -ENOENT) {
        printf("host: %s: no such name\n", argv[1]);
        return 1;
    }
    if (err < 0) {
        printf("host: %s: %s\n", argv[1], strerror(err));
        return 1;
    }

    char address[16];
    format_ip(ip, address);
    printf("%s has address %s\n", argv[1], address);
    return 0;
}
