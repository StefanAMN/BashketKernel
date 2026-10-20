/* Line editing and history for programs that read lines from the terminal
 * (the shell). The terminal is put in raw mode and every key comes here:
 *
 *   Left, Right, Home, End (and Ctrl-A, Ctrl-E)    move
 *   Backspace, Delete, Ctrl-K, Ctrl-U, Ctrl-W      erase: a character back, at
 *                                                  the cursor, to the end, to the
 *                                                  start, the word before
 *   Up, Down                                       earlier and later lines
 *   Tab                                            complete the word before the
 *                                                  cursor; a second Tab lists the
 *                                                  choices (see `complete` below)
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
#define LINEEDIT_MATCHES 64    /* Completions kept for listing (more are counted) */
#define LINEEDIT_NAME    64    /* Longest completion kept */

typedef struct lineedit lineedit_t;

/* Asked on Tab for the names that could complete `word` (the text from the
 * start of the word to the cursor). `command` is 1 if the word is where a
 * program name goes (the first of the line, or after |, ; or &). For each
 * name it calls lineedit_add_match with the whole last part of the path
 * (after the last '/' of `word`), ending in '/' for a directory. */
typedef void (*lineedit_complete_fn)(lineedit_t *e, const char *word, int command);

struct lineedit {
    char line[LINEEDIT_MAX];   /* What is being edited */
    size_t len, cursor;        /* Its length, and where the cursor is (0..len) */
    int escape;                /* Inside an escape sequence: 0 no, 1 after ESC, 2 after ESC [ */
    char escape_arg;           /* The digit of "ESC [ 3 ~" (Delete) */
    /* History: the oldest first; `browsing` is the entry shown (count: the
     * line being typed, which is kept in `typed` while browsing) */
    char history[LINEEDIT_HISTORY][LINEEDIT_MAX];
    int count, browsing;
    char typed[LINEEDIT_MAX];
    /* Completion: set `complete` to turn it on. After a Tab, `matches`
     * holds the first LINEEDIT_MATCHES names, sorted, `match_count` counts all of
     * them, and `prefix` is what they all start with. `show_matches` is set
     * when a second Tab asks for the list (lineedit_read prints it). */
    lineedit_complete_fn complete;
    char matches[LINEEDIT_MATCHES][LINEEDIT_NAME];
    int match_count, show_matches, last_was_tab;
    char prefix[LINEEDIT_NAME];
};

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

/* For the completion function: `name` is one possible completion (the
 * whole last part of the path, "/" at the end for a directory). Duplicates
 * among the names kept are ignored (past LINEEDIT_MATCHES they are
 * counted again: the count is then "at least"). */
void lineedit_add_match(lineedit_t *e, const char *name);

/* Print `prompt` and edit a line from the terminal (raw mode while
 * editing, line mode again afterwards) into `out` (size bytes). Returns its
 * length, or -1 at the end of input. */
long lineedit_read(lineedit_t *e, const char *prompt, char *out, size_t size);

#endif
