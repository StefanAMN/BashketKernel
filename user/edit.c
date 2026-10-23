/* edit: a small full-screen text editor.
 *
 *   edit FILE
 *
 *   arrows, Home, End   move                Ctrl-S   save
 *   Backspace, Delete   remove a character  Ctrl-Q   quit (asks twice if unsaved)
 *   Enter               new line
 *
 * How it works: the whole file is one array of characters with a cursor
 * position in it. Every key changes the array or the cursor, and then the
 * visible part is drawn again from scratch using terminal escape sequences
 * (ESC [ H to go to the top left, ESC [ K to clear the rest of a line, ...).
 * The terminal is switched to raw mode so that each key arrives at once,
 * without the kernel echoing or editing it.
 */
#include "ulib.h"

#define MAX_TEXT (256 * 1024)
#define CTRL(c) ((c) & 0x1F)

static char text[MAX_TEXT];
static size_t length = 0;         /* Characters in the file */
static size_t cursor = 0;         /* Insertion point, 0 .. length */
static size_t top = 0;            /* Start of the first line shown */
static unsigned columns = 80, rows = 24;
static int modified = 0;
static int partial = 0;           /* The file did not fit: only its beginning is here */
static const char *path;
static const char *message = "";  /* Shown once on the status line */

/* --- Moving around in the array ----------------------------------------- */

/* Where the line holding `pos` starts, and where it ends (its newline, or
 * the end of the text) */
static size_t line_start(size_t pos) {
    while (pos > 0 && text[pos - 1] != '\n') pos--;
    return pos;
}

/* (See line_start) */
static size_t line_end(size_t pos) {
    while (pos < length && text[pos] != '\n') pos++;
    return pos;
}

/* The same column on the previous or next line, or as near as that line goes */
static void move_vertically(int down) {
    size_t start = line_start(cursor);
    size_t column = cursor - start;
    size_t target;

    if (down) {
        size_t end = line_end(cursor);
        if (end == length) return;                /* Already on the last line */
        target = end + 1;
    } else {
        if (start == 0) return;                   /* Already on the first line */
        target = line_start(start - 1);
    }
    size_t target_end = line_end(target);
    cursor = (target + column < target_end) ? target + column : target_end;
}

/* --- Changing the text --------------------------------------------------- */

/* Put `c` in at the cursor and move past it */
static void insert_char(char c) {
    if (length >= MAX_TEXT) { message = "file is full"; return; }
    for (size_t i = length; i > cursor; i--) text[i] = text[i - 1];
    text[cursor++] = c;
    length++;
    modified = 1;
}

/* Remove the character at `pos` (nothing past the end) */
static void delete_at(size_t pos) {
    if (pos >= length) return;
    for (size_t i = pos; i + 1 < length; i++) text[i] = text[i + 1];
    length--;
    modified = 1;
}

/* --- Files ---------------------------------------------------------------- */

/* Read the file named on the command line into the text (a missing file is
 * a new one) */
static void load(void) {
    long fd = open(path, O_RDONLY);
    if (fd < 0) {
        message = (fd == -ENOENT) ? "new file" : strerror(fd);
        return;
    }
    long n;
    while (length < MAX_TEXT && (n = read((int)fd, text + length, MAX_TEXT - length)) > 0) length += (size_t)n;
    char more;
    if (length == MAX_TEXT && read((int)fd, &more, 1) > 0) {
        partial = 1;
        message = "file too large: only the beginning was loaded (it cannot be saved)";
    }
    close((int)fd);
}

/* Write the text over the file. Refused for a file that was only loaded in
 * part: saving would cut off the rest of it. */
static void save(void) {
    if (partial) {
        message = "not saved: only the beginning of this file was loaded";
        return;
    }
    long fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        message = strerror(fd);
        return;
    }
    size_t written = 0;
    while (written < length) {
        long n = write((int)fd, text + written, length - written);
        if (n <= 0) {
            close((int)fd);
            message = n < 0 ? strerror(n) : "could not write";
            return;
        }
        written += (size_t)n;
    }
    close((int)fd);
    modified = 0;
    message = "saved";
}

/* --- Drawing --------------------------------------------------------------- */

/* The frame is built in memory and sent with one write, so the screen does
 * not flicker through half-drawn states. */
static char frame[32 * 1024];
static size_t frame_len;

/* Add text, or a number in decimal, to the frame (cut off when it is full) */
static void put(const char *s) {
    while (*s && frame_len < sizeof(frame)) frame[frame_len++] = *s++;
}

/* (See put) */
static void put_number(unsigned n) {
    char digits[12];
    int count = 0;
    do { digits[count++] = (char)('0' + n % 10); n /= 10; } while (n);
    while (count && frame_len < sizeof(frame)) frame[frame_len++] = digits[--count];
}

/* Draw the visible lines, the status line and the cursor, scrolling first
 * so that the cursor's line is on the screen */
static void draw(void) {
    unsigned text_rows = rows - 1;                /* The last row is the status line */

    /* Scroll so that the cursor's line is among the rows shown */
    size_t cursor_line_start = line_start(cursor);
    if (cursor_line_start < top) top = cursor_line_start;
    for (;;) {
        unsigned distance = 0;
        for (size_t p = top; p < cursor_line_start; p = line_end(p) + 1) distance++;
        if (distance < text_rows) break;
        top = line_end(top) + 1;                  /* Move the window down a line */
    }

    frame_len = 0;
    put("\033[H");                                /* Cursor to the top left */

    size_t pos = top;
    unsigned cursor_row = 0, cursor_col = 0, line_number = 1;
    for (size_t p = 0; p < top; p++) if (text[p] == '\n') line_number++;
    unsigned cursor_line_number = line_number;

    for (unsigned row = 0; row < text_rows; row++) {
        if (pos <= length) {
            size_t end = line_end(pos);
            if (cursor >= pos && cursor <= end) {
                cursor_row = row;
                cursor_col = (unsigned)(cursor - pos);
                cursor_line_number = line_number + row;
            }
            /* A line longer than the screen is cut off at the right edge */
            for (size_t p = pos; p < end && p - pos < columns - 1 && frame_len < sizeof(frame); p++) {
                char c = text[p];
                frame[frame_len++] = (c >= 0x20 && c < 0x7F) ? c : (c == '\t' ? ' ' : '?');
            }
            pos = end + 1;                        /* Past the newline; beyond `length` after the last line */
        } else {
            put("\033[90m~\033[0m");                /* Past the end of the file */
        }
        put("\033[K\r\n");                        /* Clear the rest of the row */
    }
    if (cursor_col > columns - 1) cursor_col = columns - 1;

    /* Status line, in reverse colours */
    put("\033[30;47m ");
    put(path);
    put(modified ? " [modified]" : "");
    put("  line ");
    put_number(cursor_line_number);
    put("  ^S save  ^Q quit  ");
    put(message);
    put(" \033[K\033[0m");
    message = "";

    /* Put the terminal's cursor where ours is (rows and columns count from 1) */
    put("\033[");
    put_number(cursor_row + 1);
    put(";");
    put_number(cursor_col + 1);
    put("H");

    write(1, frame, frame_len);
}

/* --- Keys ------------------------------------------------------------------ */

/* The next byte typed: its value, or -1 at end of input. A signal interrupting
 * the read does not count: it reads again. */
static int read_key(void) {
    char c;
    for (;;) {
        long n = read(0, &c, 1);
        if (n == 1) return (unsigned char)c;
        if (n != -EINTR) return -1;               /* End of input: give up */
    }
}

/* Returns 0 when the editor should exit */
static int handle_key(int key) {
    static int quit_pressed = 0;

    if (key == CTRL('q')) {
        if (!modified || quit_pressed) return 0;
        quit_pressed = 1;
        message = "unsaved changes: ^S to save, ^Q again to discard";
        return 1;
    }
    quit_pressed = 0;

    if (key == CTRL('s')) {
        save();
    } else if (key == 0x1B) {
        /* An escape sequence from an arrow or editing key: ESC [ letter, or ESC [ digit ~ */
        if (read_key() != '[') return 1;
        int code = read_key();
        switch (code) {
            case 'A': move_vertically(0); break;
            case 'B': move_vertically(1); break;
            case 'C': if (cursor < length) cursor++; break;
            case 'D': if (cursor > 0) cursor--; break;
            case 'H': cursor = line_start(cursor); break;
            case 'F': cursor = line_end(cursor); break;
            case '3': if (read_key() == '~') delete_at(cursor); break;
            default: break;
        }
    } else if (key == '\r' || key == '\n') {
        insert_char('\n');
    } else if (key == '\b' || key == 0x7F) {
        if (cursor > 0) delete_at(--cursor);
    } else if (key == '\t' || (key >= 0x20 && key < 0x7F)) {
        insert_char((char)key);
    }
    return 1;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(2, "usage: edit FILE\n");
        return 2;
    }
    path = argv[1];
    load();

    tty_size(&columns, &rows);
    if (columns > 100) columns = 100;             /* Redrawing a huge screen for every key is slow */
    if (rows > 32) rows = 32;
    if (columns < 20 || rows < 4) {
        fprintf(2, "edit: the screen is too small\n");
        return 1;
    }

    signal(SIGINT, SIG_IGN);                      /* Ctrl-C must not throw the text away */
    tty_raw(1);
    write(1, "\033[2J", 4);

    int key;
    do {
        draw();
        key = read_key();
    } while (key >= 0 && handle_key(key));

    tty_raw(0);
    write(1, "\033[0m\033[2J\033[H", 11);
    return 0;
}
