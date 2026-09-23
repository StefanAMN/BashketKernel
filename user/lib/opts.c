/* Command-line options: see opts.h */
#include "opts.h"

void opts_init(opts_t *o) {
    o->index = 1;
    o->at = 0;
    o->value = NULL;
}

int opts_next(opts_t *o, int argc, char **argv, const char *spec, const char *prog) {
    o->value = NULL;
    if (o->at == 0) {                                   /* A new argument: is it options? */
        if (o->index >= argc) return -1;
        const char *a = argv[o->index];
        if (a[0] != '-' || a[1] == '\0') return -1;     /* An operand ("-" is one too) */
        if (a[1] == '-' && a[2] == '\0') {              /* "--": the rest are operands */
            o->index++;
            return -1;
        }
        o->at = 1;
    }
    const char *a = argv[o->index];
    char letter = a[o->at++];
    const char *found = letter != ':' ? strchr(spec, letter) : NULL;
    int takes_value = found && found[1] == ':';
    if (!found) {
        fprintf(2, "%s: unknown option -%c\n", prog, letter);
    } else if (takes_value) {
        if (a[o->at] != '\0') {                          /* Joined: "-n5" */
            o->value = a + o->at;
        } else if (o->index + 1 < argc) {                /* The next argument: "-n 5" */
            o->value = argv[++o->index];
        } else {
            fprintf(2, "%s: option -%c needs a value\n", prog, letter);
            found = NULL;
        }
    }
    if (takes_value || a[o->at] == '\0') {               /* This argument is used up */
        o->index++;
        o->at = 0;
    }
    return found ? letter : '?';
}

int opts_number(const char *text, long *out) {
    if (!text || *text == '\0') return -1;
    long n = 0;
    for (const char *c = text; *c; c++) {
        if (*c < '0' || *c > '9') return -1;
        int digit = *c - '0';
        if (n > (0x7fffffffffffffffL - digit) / 10) return -1;   /* Would pass LONG_MAX */
        n = n * 10 + digit;
    }
    *out = n;
    return 0;
}
