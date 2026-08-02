/* ping: send echo requests to a machine and report the round-trip times.
 *
 *   ping ADDRESS [COUNT]      for example: ping 10.0.2.2
 */
#include "ulib.h"

int main(int argc, char **argv) {
    uint32_t ip = argc > 1 ? parse_ip(argv[1]) : 0;
    long count = argc > 2 ? atoi(argv[2]) : 4;
    if (ip == 0 || count <= 0) {
        fprintf(2, "usage: ping ADDRESS [COUNT]   (an address looks like 10.0.2.2)\n");
        return 2;
    }

    int answered = 0;
    for (long i = 0; i < count; i++) {
        long rtt = ping(ip, 1000);
        if (rtt >= 0) {
            printf("reply from %s: time %ld.%03ld ms\n", argv[1], rtt / 1000, rtt % 1000);
            answered++;
        } else {
            printf("no reply from %s: %s\n", argv[1], strerror(rtt));
            if (rtt == -ENETDOWN) return 1;
        }
        if (i + 1 < count) sleep_ms(200);
    }

    printf("%ld sent, %d answered\n", count, answered);
    return answered > 0 ? 0 : 1;
}
