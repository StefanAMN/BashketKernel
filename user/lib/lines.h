/* Reading a file line by line, with lines of any length (up to
 * LINES_MAX_LINE), in blocks rather than a byte per system call. */
#ifndef USER_LINES_H
#define USER_LINES_H

#include "ulib.h"

#define LINES_MAX_LINE (1024 * 1024)   /* A longer line is an error (-ENOMEM), not cut short */

typedef struct {
    int fd;
    char buf[4096];            /* Read ahead: buf[pos..len) not yet used */
    size_t pos, len;
    char *line;                /* The current line (malloc'd, grown as needed), NUL-terminated */
    size_t cap;
    int ended;                 /* The file has been read to its end */
} lines_t;

/* Start reading `fd` (which stays open: the caller closes it) */
void lines_open(lines_t *r, int fd);

/* The next line, in r->line without its '\n' (a last line without one
 * counts too). Returns its length; -1 at the end of the file; -ENOMEM for
 * a line longer than LINES_MAX_LINE or memory short; a read error. */
long lines_next(lines_t *r);

/* Free the line buffer (not the descriptor) */
void lines_close(lines_t *r);

#endif
