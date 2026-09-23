/* Command-line options, read the way POSIX's utility syntax guidelines
 * describe (and getopt does): options come first, each a '-' and one
 * letter; letters may be grouped ("-in" is "-i -n"); an option that takes
 * a value has it joined ("-n5") or as the next argument ("-n 5"); "--"
 * ends the options, and so does the first argument that is not one ("-"
 * alone is an operand, standard input to most tools).
 *
 *   opts_t o;
 *   opts_init(&o);
 *   for (int c; (c = opts_next(&o, argc, argv, "in:", "grep")) != -1;) {
 *       if (c == 'n') ... o.value ...
 *       else if (c == '?') return 2;     (the message has been printed)
 *   }
 *   argv[o.index] is then the first operand. */
#ifndef USER_OPTS_H
#define USER_OPTS_H

#include "ulib.h"

typedef struct {
    int index;           /* The argument being read; after the last option, the first operand */
    int at;              /* Within argv[index]: the next letter of a group (0: start a new argument) */
    const char *value;   /* The value of the option just returned, if it takes one */
} opts_t;

/* Start reading the options of argv[1..] */
void opts_init(opts_t *o);

/* The next option letter; -1 when the options are over (o->index is then
 * the first operand); '?' for a letter not in `spec` or an option whose
 * value is missing, after printing "PROG: ..." on standard error. In
 * `spec`, a letter followed by ':' takes a value, left in o->value. */
int opts_next(opts_t *o, int argc, char **argv, const char *spec, const char *prog);

/* Read `text` as a whole decimal number from 0 to LONG_MAX into *out: 0,
 * or -1 if it is empty, has anything but digits, or is too large */
int opts_number(const char *text, long *out);

#endif
