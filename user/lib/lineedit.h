/* Line editing and history for programs that read lines from the terminal
 * (the shell). The terminal is put in raw mode and every key comes here:
 *
 *   Left, Right, Home, End (and Ctrl-A, Ctrl-E)    move
 *   Backspace, Delete, Ctrl-K, Ctrl-U, Ctrl-W      erase: a character back, at
 *                                                  the cursor, to the end, to the
 *                                                  start, the word before
 *   Up, Down                                       earlier and later lines
 *   Enter                                          done
 *   Ctrl-D                                         end of input on an empty line
 *
 * The editing (lineedit_key) is a function of the keys alone, so it can be
 * tested without a terminal; lineedit_read puts it on the screen. */
#ifndef USER_LINEEDIT_H
#define USER_LINEEDIT_H

#include "ulib.h"

#define LINEEDIT_MAX     256   /* Longest line, with its NUL */
#define LINEEDIT_HISTORY 64    /* Lines remembered */

typedef struct {
    char line[LINEEDIT_MAX];   /* What is being edited */
    size_t len, cursor;        /* Its length, and where the cursor is (0..len) */
    int escape;                /* Inside an escape sequence: 0 no, 1 after ESC, 2 after ESC [ */
    char escape_arg;           /* The digit of "ESC [ 3 ~" (Delete) */
    /* History: the oldest first; `browsing` is the entry shown (count: the
     * line being typed, which is kept in `typed` while browsing) */
    char history[LINEEDIT_HISTORY][LINEEDIT_MAX];
    int count, browsing;
    char typed[LINEEDIT_MAX];
} lineedit_t;

/* Results of lineedit_key */
#define LINEEDIT_MORE  0       /* Keep going */
#define LINEEDIT_DONE  1       /* Enter: the line is in `line` */
#define LINEEDIT_EOF   2       /* Ctrl-D on an empty line */

/* Start editing a new, empty line (the history is kept) */
void lineedit_start(lineedit_t *e);

/* Feed one key (a byte as the terminal sends it). Returns LINEEDIT_MORE,
 * LINEEDIT_DONE (the line is added to the history unless it is empty or
 * the same as the last) or LINEEDIT_EOF. */
int lineedit_key(lineedit_t *e, char key);

/* Print `prompt` and edit a line from the terminal (raw mode while
 * editing, line mode again afterwards) into `out` (size bytes). Returns its
 * length, or -1 at the end of input. */
long lineedit_read(lineedit_t *e, const char *prompt, char *out, size_t size);

#endif
