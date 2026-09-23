/* sort: print the lines of files or of standard input in order.
 *   sort [-n] [-r] [-u] [-t C] [-k N[,M]] [FILE...]
 * Byte order, or with -n by the number the key starts with (blanks, a
 * '-', digits and a fraction; a key without one counts as 0, and numbers
 * of any length compare exactly). -r reverses the order; -u prints only
 * the first of each run of lines whose keys compare equal.
 * The key is the whole line, or with -k fields N to M (to the end of the
 * line without M), fields counted from 1. With -t C, fields are separated
 * by C; without it, by runs of blanks, each field keeping the blanks
 * before it, as POSIX says (so "-k 2" on "a  x" and "a y" compares "  x"
 * with " y": use -n or -t for columns of different widths). Lines whose
 * keys compare equal keep their input order, with -r too (as GNU sort -s;
 * without -s it would then compare them byte by byte).
 * Everything is held in memory: input too large for it is refused, with
 * nothing printed, rather than sorted in part. */
#include "ulib.h"
#include "lines.h"
#include "opts.h"

typedef struct {
    char *text;
    long len;
} line_t;

static int numeric, reverse, unique;
static int separator = -1;        /* -t: the field separator, or -1 for runs of blanks */
static long first_field = 0;      /* -k: the key's first and last fields (0: the whole line / the end) */
static long last_field = 0;

/* Whether `c` is a blank (a space or a tab) */
static int blank(char c) {
    return c == ' ' || c == '\t';
}

/* Where field `n` (from 1) of `line` (len bytes) starts: an offset, or len
 * if the line has fewer fields. Without -t a field starts with the blanks
 * before it. */
static long field_start(const char *line, long len, long n) {
    long at = 0;
    for (long field = 1; field < n; field++) {
        if (separator >= 0) {
            while (at < len && line[at] != separator) at++;
            if (at == len) return len;
            at++;                                      /* Past the separator */
        } else {
            while (at < len && blank(line[at])) at++;
            while (at < len && !blank(line[at])) at++;
            if (at == len) return len;
        }
    }
    return at;
}

/* Where field `n` of `line` ends (after its last byte) */
static long field_end(const char *line, long len, long n) {
    long at = field_start(line, len, n);
    if (separator >= 0) {
        while (at < len && line[at] != separator) at++;
    } else {
        while (at < len && blank(line[at])) at++;
        while (at < len && !blank(line[at])) at++;
    }
    return at;
}

/* The key of a line, as -k asks: *start and *end (offsets into it) */
static void key_of(const line_t *l, long *start, long *end) {
    *start = 0;
    *end = l->len;
    if (first_field == 0) return;
    *start = field_start(l->text, l->len, first_field);
    if (last_field) *end = field_end(l->text, l->len, last_field);
    if (*end < *start) *end = *start;
}

/* A number at the front of a key, as -n reads it: its sign, and where its
 * digits before and after the point are (leading zeros skipped) */
typedef struct {
    int negative;
    const char *whole;
    long whole_len;
    const char *fraction;
    long fraction_len;
} number_t;

/* Read the number at the front of text[0..len) (after blanks); a key
 * without digits reads as 0 */
static number_t read_number(const char *text, long len) {
    number_t n = { 0, text, 0, text, 0 };
    long i = 0;
    while (i < len && blank(text[i])) i++;
    if (i < len && text[i] == '-') { n.negative = 1; i++; }
    while (i < len && text[i] == '0') i++;             /* "007" is 7 */
    n.whole = text + i;
    while (i < len && text[i] >= '0' && text[i] <= '9') { i++; n.whole_len++; }
    if (i < len && text[i] == '.') {
        i++;
        n.fraction = text + i;
        while (i < len && text[i] >= '0' && text[i] <= '9') { i++; n.fraction_len++; }
        while (n.fraction_len > 0 && n.fraction[n.fraction_len - 1] == '0') n.fraction_len--;  /* "1.50" is 1.5 */
    }
    if (n.whole_len == 0 && n.fraction_len == 0) n.negative = 0;  /* "-0" is 0 */
    return n;
}

/* Compare the sizes of two numbers without their signs: negative, 0 or
 * positive. Digits are compared as text, so no length overflows. */
static int compare_magnitude(const number_t *a, const number_t *b) {
    if (a->whole_len != b->whole_len) return a->whole_len < b->whole_len ? -1 : 1;
    int c = memcmp(a->whole, b->whole, (size_t)a->whole_len);
    if (c) return c;
    long common = a->fraction_len < b->fraction_len ? a->fraction_len : b->fraction_len;
    c = memcmp(a->fraction, b->fraction, (size_t)common);
    if (c) return c;
    return a->fraction_len < b->fraction_len ? -1 : a->fraction_len > b->fraction_len ? 1 : 0;
}

/* Compare the keys of two lines as asked (-n, -k, -t), before -r:
 * negative, 0 or positive */
static int compare_keys(const line_t *a, const line_t *b) {
    long as, ae, bs, be;
    key_of(a, &as, &ae);
    key_of(b, &bs, &be);
    if (numeric) {
        number_t x = read_number(a->text + as, ae - as), y = read_number(b->text + bs, be - bs);
        if (x.negative != y.negative) return x.negative ? -1 : 1;
        int c = compare_magnitude(&x, &y);
        return x.negative ? -c : c;
    }
    long an = ae - as, bn = be - bs, n = an < bn ? an : bn;
    int c = memcmp(a->text + as, b->text + bs, (size_t)n);
    if (c == 0) c = an < bn ? -1 : an > bn ? 1 : 0;
    return c;
}

/* The order of two lines: the keys' order, reversed by -r */
static int compare(const line_t *a, const line_t *b) {
    int c = compare_keys(a, b);
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

/* Read -k's value, "N" or "N,M" (fields from 1, M not before N): 0, or -1 */
static int parse_key(const char *spec) {
    char first[24];
    const char *comma = strchr(spec, ',');
    size_t n = comma ? (size_t)(comma - spec) : strlen(spec);
    if (n == 0 || n >= sizeof(first)) return -1;
    memcpy(first, spec, n);
    first[n] = '\0';
    if (opts_number(first, &first_field) < 0 || first_field < 1) return -1;
    if (comma && (opts_number(comma + 1, &last_field) < 0 || last_field < first_field)) return -1;
    return 0;
}

static line_t *lines;             /* Everything read, in input order until sorted */
static long count, cap;

/* Add the lines of `fd` to `lines`: -1 when all are in, or a negative
 * error number (-ENOMEM when they do not fit in memory) */
static long read_all(int fd) {
    static lines_t in;
    long len;
    lines_open(&in, fd);
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
    return len;
}

int main(int argc, char **argv) {
    static const char usage[] = "usage: sort [-n] [-r] [-u] [-t C] [-k N[,M]] [FILE...]\n";
    opts_t o;
    opts_init(&o);
    for (int c; (c = opts_next(&o, argc, argv, "nrut:k:", "sort")) != -1;) {
        if (c == 'n') numeric = 1;
        else if (c == 'r') reverse = 1;
        else if (c == 'u') unique = 1;
        else if (c == 't' && o.value[0] && !o.value[1]) separator = (unsigned char)o.value[0];
        else if (c == 'k' && parse_key(o.value) == 0) continue;
        else {
            if (c == 't') fprintf(2, "sort: -t takes one character\n");
            if (c == 'k') fprintf(2, "sort: -k takes N or N,M (fields from 1)\n");
            fprintf(2, "%s", usage);
            return 2;
        }
    }

    int files = argc - o.index;
    for (int i = 0; i < (files ? files : 1); i++) {
        const char *name = files ? argv[o.index + i] : NULL;
        long fd = name && strcmp(name, "-") != 0 ? open(name, O_RDONLY) : 0;
        if (fd < 0) {
            fprintf(2, "sort: %s: %s\n", name, strerror(fd));
            return 2;
        }
        long end = read_all((int)fd);
        if (fd > 0) close((int)fd);
        if (end < -1) {
            fprintf(2, "sort: %s\n", end == -ENOMEM ? "input too large to hold in memory" : strerror(end));
            return 2;
        }
    }
    line_t *spare = count ? malloc(sizeof(line_t) * (size_t)count) : NULL;
    if (count && !spare) {
        fprintf(2, "sort: input too large to hold in memory\n");
        return 2;
    }
    merge_sort(lines, spare, count);
    for (long i = 0; i < count; i++) {
        if (unique && i > 0 && compare_keys(&lines[i - 1], &lines[i]) == 0) continue;
        write(1, lines[i].text, (size_t)lines[i].len);
        write(1, "\n", 1);
    }
    return 0;
}
