/* Line editing and history (see lineedit.h) */
#include "lineedit.h"

void lineedit_start(lineedit_t *e) {
    e->len = e->cursor = 0;
    e->line[0] = '\0';
    e->escape = 0;
    e->browsing = e->count;
    e->last_was_tab = e->show_matches = 0;
}

void lineedit_add_match(lineedit_t *e, const char *name) {
    size_t n = strlen(name);
    if (n >= LINEEDIT_NAME) return;                    /* Too long to complete to */
    int kept = e->match_count < LINEEDIT_MATCHES ? e->match_count : LINEEDIT_MATCHES;
    for (int i = 0; i < kept; i++) if (strcmp(e->matches[i], name) == 0) return;
    if (e->match_count == 0) {
        memcpy(e->prefix, name, n + 1);
    } else {                                           /* Shorten the prefix to what this one shares */
        size_t i = 0;
        while (e->prefix[i] && e->prefix[i] == name[i]) i++;
        e->prefix[i] = '\0';
    }
    if (e->match_count < LINEEDIT_MATCHES) {              /* Kept sorted, for the list */
        int at = e->match_count;
        while (at > 0 && strcmp(e->matches[at - 1], name) > 0) {
            memcpy(e->matches[at], e->matches[at - 1], LINEEDIT_NAME);
            at--;
        }
        memcpy(e->matches[at], name, n + 1);
    }
    e->match_count++;
}

/* Put `text` in the line, the cursor at its end */
static void set_line(lineedit_t *e, const char *text) {
    size_t n = strlen(text);
    if (n >= LINEEDIT_MAX) n = LINEEDIT_MAX - 1;
    memcpy(e->line, text, n);
    e->line[n] = '\0';
    e->len = e->cursor = n;
}

/* Insert `c` at the cursor (nothing if the line is full) */
static void insert(lineedit_t *e, char c) {
    if (e->len + 1 >= LINEEDIT_MAX) return;
    for (size_t i = e->len; i > e->cursor; i--) e->line[i] = e->line[i - 1];
    e->line[e->cursor++] = c;
    e->line[++e->len] = '\0';
}

/* Remove `count` characters starting at `at` */
static void erase(lineedit_t *e, size_t at, size_t count) {
    if (at >= e->len || count == 0) return;
    if (count > e->len - at) count = e->len - at;
    for (size_t i = at; i + count <= e->len; i++) e->line[i] = e->line[i + count];
    e->len -= count;
    if (e->cursor > at) e->cursor = e->cursor >= at + count ? e->cursor - count : at;
}

/* Show history entry `index` (count: the line that was being typed) */
static void browse(lineedit_t *e, int index) {
    if (index < 0 || index > e->count || index == e->browsing) return;
    if (e->browsing == e->count) {                     /* Leaving the typed line: keep it */
        memcpy(e->typed, e->line, e->len + 1);
    }
    e->browsing = index;
    set_line(e, index == e->count ? e->typed : e->history[index]);
}

/* Enter: remember the line (not an empty one, nor the last one again) */
static void remember(lineedit_t *e) {
    if (e->len == 0) return;
    if (e->count > 0 && strcmp(e->history[e->count - 1], e->line) == 0) return;
    if (e->count == LINEEDIT_HISTORY) {                /* Full: the oldest goes */
        for (int i = 1; i < LINEEDIT_HISTORY; i++) memcpy(e->history[i - 1], e->history[i], LINEEDIT_MAX);
        e->count--;
    }
    memcpy(e->history[e->count++], e->line, e->len + 1);
}

/* Tab: complete the word before the cursor as far as every match agrees,
 * with a space after it if there is only one (and it is not a directory).
 * A Tab that adds nothing to a word with several matches, straight after
 * another Tab (`again`), asks for the list. */
static void complete_word(lineedit_t *e, int again) {
    /* The word: back from the cursor to a blank or an operator */
    size_t start = e->cursor;
    while (start > 0 && e->line[start - 1] != ' ' && e->line[start - 1] != '|' &&
           e->line[start - 1] != ';' && e->line[start - 1] != '&' &&
           e->line[start - 1] != '<' && e->line[start - 1] != '>') start--;
    /* A program name goes there if only blanks and nothing else, or an
     * operator that starts a command, come before it */
    size_t before = start;
    while (before > 0 && e->line[before - 1] == ' ') before--;
    int command = before == 0 || e->line[before - 1] == '|' || e->line[before - 1] == ';' || e->line[before - 1] == '&';

    char word[LINEEDIT_MAX];
    memcpy(word, e->line + start, e->cursor - start);
    word[e->cursor - start] = '\0';
    size_t tail = 0;                                   /* What the matches replace: after the last '/' */
    for (size_t i = 0; word[i]; i++) if (word[i] == '/') tail = i + 1;
    size_t typed = (e->cursor - start) - tail;

    e->match_count = 0;
    e->complete(e, word, command);
    if (e->match_count == 0) return;

    size_t added = 0;
    for (size_t i = typed; e->prefix[i]; i++, added++) insert(e, e->prefix[i]);
    if (e->match_count == 1) {
        size_t n = strlen(e->prefix);
        if (n > 0 && e->prefix[n - 1] != '/') insert(e, ' ');
    } else if (added == 0 && again) {
        e->show_matches = 1;
    }
}

int lineedit_key(lineedit_t *e, char key) {
    int again = e->last_was_tab;                       /* The key before this one was Tab */
    e->last_was_tab = (key == '\t');
    if (key == '\t') {
        if (e->complete) complete_word(e, again);
        return LINEEDIT_MORE;
    }

    /* Escape sequences: ESC [ then a letter, or ESC [ 3 ~ for Delete */
    if (e->escape == 1) {
        e->escape = key == '[' ? 2 : 0;
        e->escape_arg = 0;
        return LINEEDIT_MORE;
    }
    if (e->escape == 2) {
        if (key >= '0' && key <= '9') { e->escape_arg = key; return LINEEDIT_MORE; }
        e->escape = 0;
        switch (key) {
            case 'A': browse(e, e->browsing - 1); break;            /* Up */
            case 'B': browse(e, e->browsing + 1); break;            /* Down */
            case 'C': if (e->cursor < e->len) e->cursor++; break;   /* Right */
            case 'D': if (e->cursor > 0) e->cursor--; break;        /* Left */
            case 'H': e->cursor = 0; break;                         /* Home */
            case 'F': e->cursor = e->len; break;                    /* End */
            case '~': if (e->escape_arg == '3') erase(e, e->cursor, 1); break; /* Delete */
            default: break;
        }
        return LINEEDIT_MORE;
    }


    switch (key) {
        case 0x1B: e->escape = 1; break;
        case '\r':
        case '\n':
            remember(e);
            return LINEEDIT_DONE;
        case 0x04:                                                   /* Ctrl-D */
            if (e->len == 0) return LINEEDIT_EOF;
            erase(e, e->cursor, 1);
            break;
        case 0x7F:
        case '\b': if (e->cursor > 0) erase(e, e->cursor - 1, 1); break;
        case 0x01: e->cursor = 0; break;                             /* Ctrl-A */
        case 0x05: e->cursor = e->len; break;                        /* Ctrl-E */
        case 0x0B: erase(e, e->cursor, e->len - e->cursor); break;   /* Ctrl-K */
        case 0x15: erase(e, 0, e->cursor); break;                    /* Ctrl-U */
        case 0x17: {                                                 /* Ctrl-W: the word before the cursor */
            size_t start = e->cursor;
            while (start > 0 && e->line[start - 1] == ' ') start--;
            while (start > 0 && e->line[start - 1] != ' ') start--;
            erase(e, start, e->cursor - start);
            break;
        }
        default:
            if ((unsigned char)key >= 0x20 && (unsigned char)key < 0x7F) insert(e, key);
            break;                                                   /* Other control keys: nothing */
    }
    return LINEEDIT_MORE;
}

/* Show the line: back to the start of the row, the prompt and the line,
 * erase what an older, longer line left, and move back to the cursor */
static void redraw(const lineedit_t *e, const char *prompt) {
    printf("\r%s%s\033[K", prompt, e->line);
    if (e->len > e->cursor) printf("\033[%luD", (unsigned long)(e->len - e->cursor));
}

long lineedit_read(lineedit_t *e, const char *prompt, char *out, size_t size) {
    lineedit_start(e);
    tty_raw(1);
    printf("%s", prompt);                              /* (Nothing to erase yet) */
    int result = LINEEDIT_MORE;
    while (result == LINEEDIT_MORE) {
        char key;
        long n = read(0, &key, 1);
        if (n == -EINTR) continue;
        if (n <= 0) { result = LINEEDIT_EOF; break; }
        size_t len_before = e->len, cursor_before = e->cursor;
        result = lineedit_key(e, key);
        if (result != LINEEDIT_MORE) break;
        /* Typing at the end of the line needs only the new character
         * (on a serial line, a redraw per key is a lot of output) */
        if (e->show_matches) {                         /* The list, then the line again below it */
            e->show_matches = 0;
            printf("\n");
            int kept = e->match_count < LINEEDIT_MATCHES ? e->match_count : LINEEDIT_MATCHES;
            for (int i = 0; i < kept; i++) printf("%s%s", e->matches[i], i + 1 < kept ? "  " : "\n");
            if (e->match_count > kept) printf("(and %d more)\n", e->match_count - kept);
            redraw(e, prompt);
        } else if (cursor_before == len_before && e->len == len_before + 1 && e->cursor == e->len) {
            printf("%c", e->line[e->len - 1]);
        } else {
            redraw(e, prompt);
        }
    }
    tty_raw(0);
    printf("\n");
    if (result == LINEEDIT_EOF) return -1;
    size_t n = e->len < size - 1 ? e->len : size - 1;
    memcpy(out, e->line, n);
    out[n] = '\0';
    return (long)n;
}
