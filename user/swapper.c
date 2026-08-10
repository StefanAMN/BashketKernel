/* swapper: use more memory than the machine has.
 *
 *   swapper MEGABYTES
 *
 * Maps that much memory, writes a different value into every page, and
 * reads them all back, twice. On a machine with less RAM than that this can
 * only work if pages are pushed out to the swap disk and brought back
 * (src/mm/swap.h). Then it forks, and parent and child each check and
 * change pages that are on the disk at that moment: each must see its own.
 *
 * Exits 0 if every value was right; 2 if the kernel would not promise the
 * memory (which is the right answer on a machine without a swap disk).
 */
#include "ulib.h"

#define PAGE 4096UL

/* A number from /proc/meminfo: the first one on the line starting with `label` */
static long meminfo(const char *label, int which) {
    static char text[768];
    long fd = open("/proc/meminfo", O_RDONLY);
    if (fd < 0) return -1;
    long n = read((int)fd, text, sizeof(text) - 1);
    close((int)fd);
    if (n <= 0) return -1;
    text[n] = '\0';
    unsigned long label_len = strlen(label);
    for (char *c = text; *c; c++) {
        if ((c == text || c[-1] == '\n') && strncmp(c, label, label_len) == 0) {
            c += label_len;
            long value = -1;
            for (int found = 0; found <= which; found++) {
                while (*c && (*c < '0' || *c > '9') && *c != '\n') c++;
                if (*c < '0' || *c > '9') return -1;
                value = 0;
                while (*c >= '0' && *c <= '9') value = value * 10 + (*c++ - '0');
            }
            return value;
        }
    }
    return -1;
}

/* What page `i` holds, in generation `g` */
static unsigned long value_of(unsigned long i, unsigned long g) {
    return (i * 0x9E3779B97F4A7C15UL) ^ (g << 56) ^ 0x5A5A5A5AUL;
}

static unsigned long check(const unsigned long *memory, unsigned long pages, unsigned long generation) {
    unsigned long wrong = 0;
    for (unsigned long i = 0; i < pages; i++) {
        const unsigned long *page = memory + i * (PAGE / sizeof(unsigned long));
        /* The start, the middle and the end of every page */
        if (page[0] != value_of(i, generation) || page[255] != ~value_of(i, generation) || page[511] != i) wrong++;
    }
    return wrong;
}

static void fill(unsigned long *memory, unsigned long pages, unsigned long generation) {
    for (unsigned long i = 0; i < pages; i++) {
        unsigned long *page = memory + i * (PAGE / sizeof(unsigned long));
        page[0] = value_of(i, generation);
        page[255] = ~value_of(i, generation);
        page[511] = i;
    }
}

int main(int argc, char **argv) {
    unsigned long megabytes = argc > 1 ? (unsigned long)atoi(argv[1]) : 0;
    if (megabytes == 0 || megabytes > 2048) {
        fprintf(2, "usage: swapper MEGABYTES\n");
        return 1;
    }
    unsigned long pages = megabytes * 256;
    long out_before = meminfo("Swapped:", 0), in_before = meminfo("Swapped:", 1);

    unsigned long *memory = mmap_anon(pages * PAGE);
    if (!memory) {
        printf("swapper: the kernel will not promise %lu MiB\n", megabytes);
        return 2;
    }

    fill(memory, pages, 1);
    unsigned long wrong = check(memory, pages, 1);
    fill(memory, pages, 2);               /* Pages that came back are changed and go out again */
    wrong += check(memory, pages, 2);
    if (wrong) {
        printf("swapper: FAILED, %lu pages did not hold what was written\n", wrong);
        return 1;
    }

    /* The first pages have long been pushed out. The child changes them;
     * the parent must still find its own values there, and the child its. */
    unsigned long sample = pages < 512 ? pages : 512;
    long child = fork();
    if (child < 0) {
        printf("swapper: FAILED, cannot fork\n");
        return 1;
    }
    if (child == 0) {
        if (check(memory, sample, 2) != 0) exit(3);
        fill(memory, sample, 7);
        exit(check(memory, sample, 7) == 0 ? 0 : 4);
    }
    int status = -1;
    if (waitpid(child, &status, 0) != child || status != 0) {
        printf("swapper: FAILED, the child saw the wrong memory (status %d)\n", status);
        return 1;
    }
    if (check(memory, pages, 2) != 0) {
        printf("swapper: FAILED, the child's writes showed in the parent\n");
        return 1;
    }

    /* A system call into memory that is on the disk: the kernel has to
     * bring the pages back before it can use them as a buffer */
    int ends[2];
    char *first = (char *)memory;
    if (pipe(ends) != 0 || write(ends[1], "from the pipe", 13) != 13 || read(ends[0], first + 8, 13) != 13 ||
        strncmp(first + 8, "from the pipe", 13) != 0 || memory[0] != value_of(0, 2)) {
        printf("swapper: FAILED, a read() into swapped memory went wrong\n");
        return 1;
    }
    close(ends[0]);
    close(ends[1]);

    long out = meminfo("Swapped:", 0) - out_before, in = meminfo("Swapped:", 1) - in_before;
    munmap(memory, pages * PAGE);
    printf("swapper: %lu MiB written and read back; %ld pages went out to disk, %ld came back\n", megabytes, out, in);
    return 0;
}
