/* nc: connect standard input and output to a TCP connection.
 *
 *   nc ADDRESS PORT      connect to a server
 *   nc -l PORT           wait for one connection to PORT
 *
 * What is typed (or piped in) goes to the other end; what arrives is
 * printed. End of input (Ctrl-D) finishes our half of the conversation;
 * nc exits when the other end has finished too.
 */
#include "ulib.h"

int main(int argc, char **argv) {
    bool server = argc == 3 && strcmp(argv[1], "-l") == 0;
    uint32_t ip = (!server && argc == 3) ? parse_ip(argv[1]) : 0;
    long port = argc == 3 ? atoi(argv[2]) : 0;
    if (argc != 3 || port <= 0 || port > 65535 || (!server && ip == 0)) {
        fprintf(2, "usage: nc ADDRESS PORT\n       nc -l PORT\n");
        return 2;
    }

    long fd = socket(SOCK_STREAM);
    long err = fd < 0 ? fd : 0;
    if (err == 0 && server) {
        err = bind((int)fd, (unsigned)port);
        if (err == 0) err = listen((int)fd, 1);
        if (err == 0) {
            long conn = accept((int)fd, NULL);
            close((int)fd);
            fd = conn;
            if (conn < 0) err = conn;
        }
    } else if (err == 0) {
        err = connect((int)fd, ip, (unsigned)port);
    }
    if (err < 0) {
        fprintf(2, "nc: %s\n", strerror(err));
        return 1;
    }

    /* Both directions in one process: wait for whichever side has
     * something, and pass it on. Once our input ends, the other end is told
     * (shutdown) and only the connection is watched. */
    static char buf[4096];
    struct pollfd fds[2] = { { 0, POLLIN, 0 }, { (int)fd, POLLIN, 0 } };
    long result = 0;
    for (;;) {
        long ready = poll(fds, 2, -1);
        if (ready == -EINTR) continue;
        if (ready < 0) { result = ready; break; }

        if (fds[0].revents) {
            long n = read(0, buf, sizeof(buf));
            if (n > 0) {
                for (long sent = 0; sent < n; ) {
                    long w = write((int)fd, buf + sent, (size_t)(n - sent));
                    if (w <= 0) { result = w < 0 ? w : -EPIPE; break; }
                    sent += w;
                }
                if (result < 0) break;
            } else if (n != -EINTR) {
                shutdown((int)fd);      /* The other end sees end-of-file; we still listen */
                fds[0].fd = -1;
            }
        }
        if (fds[1].revents) {
            long n = read((int)fd, buf, sizeof(buf));
            if (n > 0) write(1, buf, (size_t)n);
            else if (n != -EINTR) { result = n; break; }   /* The other end is done (0), or failed */
        }
    }
    close((int)fd);
    if (result < 0) {
        fprintf(2, "nc: %s\n", strerror(result));
        return 1;
    }
    return 0;
}
