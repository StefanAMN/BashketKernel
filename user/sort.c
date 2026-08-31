/* sort: print the lines of a file or of standard input in order.
 *   sort [-r] [-n] [FILE]
 * Byte order, or with -n by the number each line starts with (lines
 * without one count as 0); -r reverses it. Lines that compare equal keep
 * their order, with -r too (as GNU sort -s does; without -s it would
 * compare them byte by byte).
 * Everything is held in memory: input too large for it is refused, with
 * nothing printed, rather than sorted in part. */
#include "ulib.h"
#include "lines.h"

typedef struct {
    char *text;
    long len;
} line_t;

static int numeric, reverse;

/* Compare two lines as asked (-n, -r): negative, 0 or positive */
static int compare(const line_t *a, const line_t *b) {
    int c;
    if (numeric) {
        long x = atoi(a->text), y = atoi(b->text);
        c = x < y ? -1 : x > y ? 1 : 0;
    } else {
        long n = a->len < b->len ? a->len : b->len;
        c = memcmp(a->text, b->text, (size_t)n);
        if (c == 0) c = a->len < b->len ? -1 : a->len > b->len ? 1 : 0;
    }
    return reverse ? -c : c;
}

/* Merge sort lines[0..count) using `spare` (as large): stable, and
 * n log n whatever the input */
static void merge_sort(line_t *lines, line_t *spare, long count) {
    if (count < 2) return;
    long half = count / 2;
    merge_sort(lines, spare, half);
    merge_sort(lines + half, spare, count - half);
    long i = 0, j = half, k = 0;
    while (i < half && j < count) spare[k++] = compare(&lines[j], &lines[i]) < 0 ? lines[j++] : lines[i++];
    while (i < half) spare[k++] = lines[i++];
    while (j < count) spare[k++] = lines[j++];
    memcpy(lines, spare, sizeof(line_t) * (size_t)count);
}

int main(int argc, char **argv) {
    int arg = 1;
    for (; arg < argc && argv[arg][0] == '-' && argv[arg][1] != '\0'; arg++) {
        if (strcmp(argv[arg], "-r") == 0) reverse = 1;
        else if (strcmp(argv[arg], "-n") == 0) numeric = 1;
        else {
            fprintf(2, "usage: sort [-r] [-n] [FILE]\n");
            return 2;
        }
    }
    long fd = arg < argc ? open(argv[arg], O_RDONLY) : 0;
    if (fd < 0) {
        fprintf(2, "sort: %s: %s\n", argv[arg], strerror(fd));
        return 2;
    }

    line_t *lines = NULL;
    long count = 0, cap = 0, len;
    static lines_t in;
    lines_open(&in, (int)fd);
    while ((len = lines_next(&in)) >= 0) {
        if (count == cap) {                            /* Twice the room */
            long bigger = cap ? cap * 2 : 256;
            line_t *grown = malloc(sizeof(line_t) * (size_t)bigger);
            if (!grown) { len = -ENOMEM; break; }
            if (lines) memcpy(grown, lines, sizeof(line_t) * (size_t)count);
            free(lines);
            lines = grown;
            cap = bigger;
        }
        char *copy = malloc((size_t)len + 1);
        if (!copy) { len = -ENOMEM; break; }
        memcpy(copy, in.line, (size_t)len + 1);
        lines[count].text = copy;
        lines[count++].len = len;
    }
    lines_close(&in);
    line_t *spare = (len == -1 && count) ? malloc(sizeof(line_t) * (size_t)count) : NULL;
    if (len < -1 || (count && !spare)) {
        fprintf(2, "sort: %s\n", len == -ENOMEM || !spare ? "input too large to hold in memory" : strerror(len));
        return 2;
    }
    merge_sort(lines, spare, count);
    for (long i = 0; i < count; i++) {
        write(1, lines[i].text, (size_t)lines[i].len);
        write(1, "\n", 1);
    }
    return 0;
}
