#include "test.h"
#include "ansi.h"
#include "console.h"
#include "kstring.h"

// Feed a string; return the last action that was not ANSI_NOTHING (or
// ANSI_NOTHING if there was none) and count the printed characters.
static ansi_action_t feed(ansi_parser_t *p, const char *text, int *printed) {
    ansi_action_t last = { .kind = ANSI_NOTHING };
    for (; *text; text++) {
        ansi_action_t a = ansi_feed(p, *text);
        if (a.kind == ANSI_PRINT && printed) (*printed)++;
        if (a.kind != ANSI_NOTHING) last = a;
    }
    return last;
}

TEST(ansi_parses_sequences) {
    ansi_parser_t p;
    ansi_init(&p);
    int printed = 0;

    // Plain text passes straight through
    ansi_action_t a = feed(&p, "abc", &printed);
    EXPECT_EQ(printed, 3);
    EXPECT_TRUE(a.kind == ANSI_PRINT && a.ch == 'c');

    // A colour sequence produces one action and prints nothing
    printed = 0;
    a = feed(&p, "\033[31m", &printed);
    EXPECT_EQ(printed, 0);
    EXPECT_TRUE(a.kind == ANSI_SGR && a.param_count == 1 && a.params[0] == 31);

    // Several parameters; an empty one counts as 0
    a = feed(&p, "\033[1;33;44m", NULL);
    EXPECT_TRUE(a.kind == ANSI_SGR && a.param_count == 3);
    EXPECT_TRUE(a.params[0] == 1 && a.params[1] == 33 && a.params[2] == 44);
    a = feed(&p, "\033[m", NULL);
    EXPECT_TRUE(a.kind == ANSI_SGR && a.param_count == 0);
    a = feed(&p, "\033[;5m", NULL);
    EXPECT_TRUE(a.kind == ANSI_SGR && a.param_count == 2 && a.params[0] == 0 && a.params[1] == 5);

    // Cursor and erase commands
    a = feed(&p, "\033[12;40H", NULL);
    EXPECT_TRUE(a.kind == ANSI_CURSOR_TO && a.params[0] == 12 && a.params[1] == 40);
    EXPECT_TRUE(feed(&p, "\033[H", NULL).kind == ANSI_CURSOR_TO);
    EXPECT_TRUE(feed(&p, "\033[3A", NULL).kind == ANSI_CURSOR_UP);
    EXPECT_TRUE(feed(&p, "\033[B", NULL).kind == ANSI_CURSOR_DOWN);
    EXPECT_TRUE(feed(&p, "\033[7C", NULL).kind == ANSI_CURSOR_RIGHT);
    EXPECT_TRUE(feed(&p, "\033[2D", NULL).kind == ANSI_CURSOR_LEFT);
    a = feed(&p, "\033[2J", NULL);
    EXPECT_TRUE(a.kind == ANSI_ERASE_DISPLAY && a.params[0] == 2);
    EXPECT_TRUE(feed(&p, "\033[K", NULL).kind == ANSI_ERASE_LINE);

    // Text directly after a sequence is text again
    printed = 0;
    a = feed(&p, "\033[0mok", &printed);
    EXPECT_EQ(printed, 2);
    EXPECT_TRUE(a.kind == ANSI_PRINT && a.ch == 'k');
}

TEST(ansi_survives_malformed_input) {
    ansi_parser_t p;
    ansi_init(&p);
    int printed = 0;

    // Unknown commands and private sequences are swallowed whole
    feed(&p, "\033[?25l\033[5n\033[1;2;3z", &printed);
    EXPECT_EQ(printed, 0);

    // ESC not followed by '[' is dropped together with the next character
    printed = 0;
    feed(&p, "\033Xy", &printed);
    EXPECT_EQ(printed, 1);

    // A doubled ESC still starts a sequence
    ansi_action_t a = feed(&p, "\033\033[32m", NULL);
    EXPECT_TRUE(a.kind == ANSI_SGR && a.params[0] == 32);

    // More parameters than there is room for: the extra ones are dropped
    a = feed(&p, "\033[1;2;3;4;5;6;7;8;9;10;11;12m", NULL);
    EXPECT_TRUE(a.kind == ANSI_SGR && a.param_count == ANSI_MAX_PARAMS && a.params[7] == 8);

    // A number far too long does not overflow
    a = feed(&p, "\033[99999999999999999999999999999999H", NULL);
    EXPECT_TRUE(a.kind == ANSI_CURSOR_TO && a.params[0] > 0 && a.params[0] < 1000000);

    // A newline in the middle abandons the sequence; later text is normal
    printed = 0;
    feed(&p, "\033[3", NULL);
    feed(&p, "\n", NULL);
    feed(&p, "text", &printed);
    EXPECT_EQ(printed, 4);

    // A sequence cut off at the end of output leaves the parser waiting, and
    // the next sequence character completes it
    feed(&p, "\033[4", NULL);
    a = feed(&p, "1m", NULL);
    EXPECT_TRUE(a.kind == ANSI_SGR && a.params[0] == 41);

    EXPECT_EQ(ansi_color(1), 0xAA0000u);
    EXPECT_EQ(ansi_color(15), 0xFFFFFFu);
    EXPECT_EQ(ansi_color(-1), 0xFFFFFFu);
    EXPECT_EQ(ansi_color(16), 0xFFFFFFu);
}

// Does the 8x8 cell at (column, row) contain a pixel of this colour?
static bool cell_has(uint32_t column, uint32_t row, uint32_t color) {
    for (uint32_t y = 0; y < 8; y++) {
        for (uint32_t x = 0; x < 8; x++) {
            if (console_get_pixel(column * 8 + x, row * 8 + y) == color) return true;
        }
    }
    return false;
}

static bool cell_is_only(uint32_t column, uint32_t row, uint32_t color) {
    for (uint32_t y = 0; y < 8; y++) {
        for (uint32_t x = 0; x < 8; x++) {
            if (console_get_pixel(column * 8 + x, row * 8 + y) != color) return false;
        }
    }
    return true;
}

// The console acting on the sequences: checked by looking at the pixels.
TEST(console_obeys_escape_sequences) {
    uint32_t column, row;

    // Clear the screen and go to the top left
    console_write("\033[2J\033[H");
    console_get_cursor(&column, &row);
    EXPECT_TRUE(column == 0 && row == 0);
    EXPECT_TRUE(cell_is_only(0, 0, 0x000000) && cell_is_only(50, 20, 0x000000));

    // Red text: the cell has red pixels and the cursor has moved on
    console_write("\033[31mX");
    EXPECT_TRUE(cell_has(0, 0, 0xAA0000));
    console_get_cursor(&column, &row);
    EXPECT_TRUE(column == 1 && row == 0);

    // Green background, bright yellow text, at row 5 column 10 (1-based)
    console_write("\033[5;10H\033[42;93mY");
    EXPECT_TRUE(cell_has(9, 4, 0x00AA00) && cell_has(9, 4, 0xFFFF55));
    EXPECT_TRUE(!cell_has(9, 4, 0x000000));

    // Reset, then relative movement: up 2, right 3 from (10, 4)
    console_write("\033[0m\033[2A\033[3CZ");
    EXPECT_TRUE(cell_has(13, 2, 0xFFFFFF));
    console_get_cursor(&column, &row);
    EXPECT_TRUE(column == 14 && row == 2);

    // Movement stops at the edges
    console_write("\033[999A\033[999D");
    console_get_cursor(&column, &row);
    EXPECT_TRUE(column == 0 && row == 0);
    console_write("\033[9999;9999H");
    console_get_cursor(&column, &row);
    EXPECT_TRUE(column > 50 && row > 20);          // The last cell, wherever that is

    // Erase to the end of the line removes Y but not X
    console_write("\033[5;1H\033[K");
    EXPECT_TRUE(cell_is_only(9, 4, 0x000000));
    EXPECT_TRUE(cell_has(0, 0, 0xAA0000));

    // Leave the screen clean for the tests that follow
    console_write("\033[0m\033[2J\033[H");
    EXPECT_TRUE(cell_is_only(0, 0, 0x000000));
}

void test_suite_ansi(void) {
    kprintf("Suite: ANSI Escape Sequences\n");
    RUN_TEST(ansi_parses_sequences);
    RUN_TEST(ansi_survives_malformed_input);
    RUN_TEST(console_obeys_escape_sequences);
}
