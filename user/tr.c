/* tr: change or delete characters, from standard input to standard output.
 *   tr SET1 SET2      each character of SET1 becomes the one at the same
 *                     place in SET2 (the last of SET2 is repeated if it is
 *                     shorter: "tr a-z x" turns every letter into x)
 *   tr -d SET1        delete every character of SET1
 * A set lists characters, with ranges ("a-z", "0-9") and the escapes \n,
 * \t and \\. A character named twice in SET1 takes its last mapping.
 * Works on bytes, so lines (and the input) may be of any length. */
#include "ulib.h"

#define SET_MAX 256

/* Expand `text` (ranges and escapes) into `out`, which holds SET_MAX
 * bytes. Returns how many it holds, or -1 if the set is malformed (a range
 * that runs backwards, a lone backslash at the end) or too long. */
static int expand_set(const char *text, unsigned char *out) {
    int n = 0;
    while (*text) {
        unsigned char c = (unsigned char)*text++;
        if (c == '\\') {
            if (*text == 'n') c = '\n';
            else if (*text == 't') c = '\t';
            else if (*text == '\\') c = '\\';
            else return -1;
            text++;
        }
        if (text[0] == '-' && text[1]) {        /* "c-d": a range */
            unsigned char last = (unsigned char)text[1];
            if (last == '\\') return -1;        /* (Keep ranges to plain characters) */
            if (last < c) return -1;
            text += 2;
            for (int x = c; x <= last; x++) {
                if (n >= SET_MAX) return -1;
                out[n++] = (unsigned char)x;
            }
            continue;
        }
        if (n >= SET_MAX) return -1;
        out[n++] = c;
    }
    return n;
}

int main(int argc, char **argv) {
    int delete = argc == 3 && strcmp(argv[1], "-d") == 0;
    static unsigned char set1[SET_MAX], set2[SET_MAX];
    int n1 = -1, n2 = -1;
    if (delete) {
        n1 = expand_set(argv[2], set1);
    } else if (argc == 3 && argv[1][0] != '-') {
        n1 = expand_set(argv[1], set1);
        n2 = expand_set(argv[2], set2);
    }
    if (n1 <= 0 || (!delete && n2 <= 0)) {
        fprintf(2, "usage: tr SET1 SET2  or  tr -d SET1   (from standard input)\n");
        return 2;
    }

    /* What each byte becomes, and whether it is dropped */
    static unsigned char map[256];
    static unsigned char drop[256];
    for (int c = 0; c < 256; c++) map[c] = (unsigned char)c;
    for (int i = 0; i < n1; i++) {
        if (delete) drop[set1[i]] = 1;
        else map[set1[i]] = set2[i < n2 ? i : n2 - 1];
    }

    static unsigned char buf[4096];
    long got;
    while ((got = read(0, buf, sizeof(buf))) > 0) {
        long kept = 0;
        for (long i = 0; i < got; i++) {
            if (!drop[buf[i]]) buf[kept++] = map[buf[i]];
        }
        if (kept && write(1, buf, (size_t)kept) != kept) return 1;
    }
    if (got < 0) {
        fprintf(2, "tr: %s\n", strerror(got));
        return 1;
    }
    return 0;
}
