/* date: show the time of day, or set it.
 *
 *   date                          print it (UTC)
 *   date -s "YYYY-MM-DD HH:MM[:SS]"   set it (root only); the clock in the
 *                                 machine's CMOS chip is not changed
 */
#include "ulib.h"

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "-s") == 0) {
        unsigned long seconds;
        if (parse_date(argv[2], &seconds) != 0) {
            fprintf(2, "date: '%s' is not a date: write YYYY-MM-DD HH:MM[:SS]\n", argv[2]);
            return 1;
        }
        long err = set_time(seconds);
        if (err < 0) {
            fprintf(2, "date: cannot set the time: %s\n", strerror(err));
            return 1;
        }
    } else if (argc != 1) {
        fprintf(2, "usage: date [-s \"YYYY-MM-DD HH:MM[:SS]\"]\n");
        return 2;
    }
    char text[20];
    format_date(time_ns() / 1000000000UL, text);
    printf("%s UTC\n", text);
    return 0;
}
