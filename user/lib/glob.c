/* Shell wildcards (see glob.h) */
#include "glob.h"

int glob_has_wildcard(const char *word) {
    for (; *word; word++) {
        if (*word == '*' || *word == '?' || *word == '[') return 1;
    }
    return 0;
}

/* The bracket expression starting at `p` (just after '['): whether `c` is
 * in it. Sets *end to the character after the closing ']', or returns -1
 * if there is none (the '[' is then an ordinary character). A ']' first in
 * the list is a member, not the end. */
static int bracket(const char *p, char c, const char **end) {
    int negate = (*p == '!');
    if (negate) p++;
    int found = 0;
    const char *first = p;
    for (; *p && (*p != ']' || p == first); p++) {
        if (p[1] == '-' && p[2] && p[2] != ']') {           /* A range, a-z */
            if (c >= p[0] && c <= p[2]) found = 1;
            p += 2;
        } else if (*p == c) {
            found = 1;
        }
    }
    if (*p != ']') return -1;
    *end = p + 1;
    return found != negate;
}

/* The matching itself, without the rule for hidden names. A '*' is tried
 * against every possible length, shortest first. Only the last star seen
 * (`star`) is ever retried, which is enough (a later star can absorb
 * whatever an earlier one would have), so there is no exponential search. */
static int match(const char *p, const char *n) {
    const char *star = 0, *star_name = 0;
    while (*n) {
        if (*p == '*') {
            star = ++p;
            star_name = n;
            continue;
        }
        if (*p == '[') {
            const char *end;
            int in = bracket(p + 1, *n, &end);
            if (in == 1) { p = end; n++; continue; }
            if (in == -1 && *n == '[') { p++; n++; continue; }  /* Unclosed: literal */
        } else if (*p && (*p == '?' || *p == *n)) {
            p++;
            n++;
            continue;
        }
        if (!star) return 0;
        p = star;                                               /* The star takes one more */
        n = ++star_name;
    }
    while (*p == '*') p++;
    return *p == '\0';
}

int glob_match(const char *pattern, const char *name) {
    if (name[0] == '.' && pattern[0] != '.') return 0;
    return match(pattern, name);
}

int glob_match_text(const char *pattern, const char *text) {
    return match(pattern, text);
}
