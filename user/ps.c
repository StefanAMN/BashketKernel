/* ps: list the tasks in the system (the kernel provides the list as text). */
#include "ulib.h"

int main(void) {
    long fd = open("/proc/tasks", O_RDONLY);
    if (fd < 0) {
        fprintf(2, "ps: /proc/tasks: %s\n", strerror(fd));
        return 1;
    }

    char buf[512];
    long n;
    while ((n = read((int)fd, buf, sizeof(buf))) > 0) write(1, buf, (size_t)n);
    close((int)fd);
    return n < 0 ? 1 : 0;
}
