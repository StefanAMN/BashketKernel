/* grep: print the lines that contain a text.
 *   grep [-i] [-n] [-c] [-l] [-v] TEXT [FILE...]
 * The text is matched as it is (no regular expressions). -i ignores the
 * case of ASCII letters; -v selects the lines that do not contain it; -n
 * puts each line's number before it; -c prints only how many lines were
 * selected; -l prints only the names of the files with a selected line
 * (and stops reading each at its first; it wins over -c, as in GNU grep).
 * Options may be grouped (-in) and come before the text (user/lib/opts.h).
 * With several files each line or count is preceded by its file's name.
 * Exit status: 0 if a line was selected, 1 if none was, 2 on an error. */
#include "ulib.h"
#include "lines.h"
#include "opts.h"

static int ignore_case, invert, numbered, count_only, names_only;

/* `c` in lower case if it is an ASCII capital, as it is otherwise */
static char folded(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

/* Whether `line` (len bytes, may hold NULs) contains `text` (n bytes),
 * ignoring case if -i was given */
static int contains(const char *line, long len, const char *text, long n) {
    for (long i = 0; i + n <= len; i++) {
        long j = 0;
        if (ignore_case) {
            while (j < n && folded(line[i + j]) == folded(text[j])) j++;
        } else {
            while (j < n && line[i + j] == text[j]) j++;
        }
        if (j == n) return 1;
    }
    return 0;
}

/* Search one open file; prints as asked. The number of lines selected
 * (with -l, 1 at the first and no more), or a negative error number. */
static long search(int fd, const char *name, const char *text) {
    static lines_t in;
    lines_open(&in, fd);
    long n = (long)strlen(text), selected = 0, number = 0, len;
    while ((len = lines_next(&in)) >= 0) {
        number++;
        if (contains(in.line, len, text, n) == invert) continue;
        selected++;
        if (names_only) break;
        if (count_only) continue;
        if (name) printf("%s:", name);
        if (numbered) printf("%ld:", number);
        write(1, in.line, (size_t)len);                /* (printf would stop at a NUL) */
        write(1, "\n", 1);
    }
    lines_close(&in);
    if (len < -1) return len;
    if (names_only) {
        if (selected) printf("%s\n", name ? name : "(standard input)");
    } else if (count_only) {
        if (name) printf("%s:", name);
        printf("%ld\n", selected);
    }
    return selected;
}

int main(int argc, char **argv) {
    opts_t o;
    opts_init(&o);
    for (int c; (c = opts_next(&o, argc, argv, "invcl", "grep")) != -1;) {
        if (c == 'i') ignore_case = 1;
        else if (c == 'n') numbered = 1;
        else if (c == 'v') invert = 1;
        else if (c == 'c') count_only = 1;
        else if (c == 'l') names_only = 1;
        else o.index = argc;                           /* An unknown option: the usage, below */
        if (o.index >= argc) break;
    }
    if (o.index >= argc) {
        fprintf(2, "usage: grep [-i] [-n] [-c] [-l] [-v] TEXT [FILE...]\n");
        return 2;
    }
    int arg = o.index;
    const char *text = argv[arg++];
    int files = argc - arg, status = 1;
    if (files == 0) {
        long found = search(0, NULL, text);
        if (found < 0) { fprintf(2, "grep: %s\n", strerror(found)); return 2; }
        return found > 0 ? 0 : 1;
    }
    for (; arg < argc; arg++) {
        long fd = open(argv[arg], O_RDONLY);
        /* -l names the file even when it is the only one */
        const char *shown = files > 1 || names_only ? argv[arg] : NULL;
        long found = fd >= 0 ? search((int)fd, shown, text) : fd;
        if (fd >= 0) close((int)fd);
        if (found < 0) {
            fprintf(2, "grep: %s: %s\n", argv[arg], strerror(found));
            status = 2;
        } else if (found > 0 && status == 1) {
            status = 0;
        }
    }
    return status;
}
