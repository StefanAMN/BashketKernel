/* seq: print a sequence of whole numbers, one per line.
 *   seq LAST               1, 2, ... LAST
 *   seq FIRST LAST         FIRST, FIRST+1, ... LAST
 *   seq FIRST STEP LAST    FIRST, FIRST+STEP, ... up to LAST (down to it
 *                          for a negative STEP)
 * Nothing is printed when LAST is already passed (seq 0 prints nothing).
 * Numbers may be negative; anything that is not a whole number, and a
 * STEP of 0, is refused with status 2. Not POSIX (which has no seq): the
 * forms are those of the common implementations, without fractions.
 */
#include "ulib.h"

/* Read the whole of `text` as a decimal number (an optional '-' or '+',
 * then digits, nothing after them) into *out. 1 if it is one, 0 if not
 * (or if it does not fit in 18 digits, so that steps cannot overflow). */
static int parse_number(const char *text, long *out) {
    const char *c = text;
    int negative = (*c == '-');
    if (*c == '-' || *c == '+') c++;
    if (*c == '\0') return 0;
    long value = 0;
    int digits = 0;
    for (; *c; c++) {
        if (*c < '0' || *c > '9' || ++digits > 18) return 0;
        value = value * 10 + (*c - '0');
    }
    *out = negative ? -value : value;
    return 1;
}

int main(int argc, char **argv) {
    long first = 1, step = 1, last = 0;
    int ok = 1;
    if (argc == 2) {
        ok = parse_number(argv[1], &last);
    } else if (argc == 3) {
        ok = parse_number(argv[1], &first) && parse_number(argv[2], &last);
    } else if (argc == 4) {
        ok = parse_number(argv[1], &first) && parse_number(argv[2], &step) && parse_number(argv[3], &last);
    } else {
        ok = 0;
    }
    if (!ok || step == 0) {
        fprintf(2, "usage: seq [FIRST [STEP]] LAST (whole numbers, STEP not 0)\n");
        return 2;
    }
    for (long n = first; step > 0 ? n <= last : n >= last; n += step) printf("%ld\n", n);
    return 0;
}
