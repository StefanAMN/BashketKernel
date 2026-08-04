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

    /* Two directions at once need two processes: the child carries input to
     * the connection, this one carries the connection to the output. */
    long writer = fork();
    if (writer < 0) {
        fprintf(2, "nc: %s\n", strerror(writer));
        return 1;
    }
    if (writer == 0) {
        static char buf[4096];
        long n;
        while ((n = read(0, buf, sizeof(buf))) > 0) {
            long sent = 0;
            while (sent < n) {
                long w = write((int)fd, buf + sent, (size_t)(n - sent));
                if (w <= 0) exit(1);
                sent += w;
            }
        }
        shutdown((int)fd); /* The other end sees end-of-file; we still listen */
        exit(0);
    }

    static char buf[4096];
    long n;
    while ((n = read((int)fd, buf, sizeof(buf))) > 0) write(1, buf, (size_t)n);

    /* The other end is done. Input may still be waiting at the keyboard. */
    kill((int)writer, 9);
    int status;
    waitpid(writer, &status);
    close((int)fd);
    if (n < 0) {
        fprintf(2, "nc: %s\n", strerror(n));
        return 1;
    }
    return 0;
}
