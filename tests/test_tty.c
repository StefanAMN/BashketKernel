#include "test.h"
#include "tty.h"
#include "kstring.h"

// Type a string into the terminal, one key at a time
static bool type(const char *keys) {
    bool ready = false;
    while (*keys) ready = tty_input(*keys++);
    return ready;
}

TEST(tty_delivers_whole_edited_lines) {
    char buf[TTY_LINE_MAX + 8];
    tty_init();

    // Nothing is readable until Enter
    EXPECT_TRUE(!type("helo"));
    EXPECT_TRUE(!type("\b"));             // Backspace removes the 'o'
    EXPECT_TRUE(type("lo\n"));
    EXPECT_EQ(tty_read(buf, sizeof(buf)), 6);
    EXPECT_EQ(memcmp(buf, "hello\n", 6), 0);

    // Backspace on an empty line does nothing; DEL works like backspace
    EXPECT_TRUE(type("\b\bab\x7f" "c\r"));  // Carriage return ends a line too
    EXPECT_EQ(tty_read(buf, sizeof(buf)), 3);
    EXPECT_EQ(memcmp(buf, "ac\n", 3), 0);

    // Ctrl-U throws the line away
    EXPECT_TRUE(type("garbage\x15" "ok\n"));
    EXPECT_EQ(tty_read(buf, sizeof(buf)), 3);
    EXPECT_EQ(memcmp(buf, "ok\n", 3), 0);
}

TEST(tty_short_reads_and_end_of_file) {
    char buf[16];
    tty_init();

    // A read smaller than the line gets part of it; the rest waits
    EXPECT_TRUE(type("abcdef\n"));
    EXPECT_EQ(tty_read(buf, 4), 4);
    EXPECT_EQ(memcmp(buf, "abcd", 4), 0);
    EXPECT_TRUE(type("XYZ"));             // Typed too early: dropped, the line is still pending
    EXPECT_EQ(tty_read(buf, 16), 3);
    EXPECT_EQ(memcmp(buf, "ef\n", 3), 0);

    // Ctrl-D on an empty line is end-of-file ...
    EXPECT_TRUE(type("\x04"));
    EXPECT_EQ(tty_read(buf, sizeof(buf)), 0);

    // ... and after some text it delivers the text without a newline
    EXPECT_TRUE(type("part\x04"));
    EXPECT_EQ(tty_read(buf, sizeof(buf)), 4);
    EXPECT_EQ(memcmp(buf, "part", 4), 0);

    EXPECT_EQ(tty_read(buf, 0), 0);       // A zero-length read never waits
}

TEST(tty_line_length_is_bounded) {
    static char buf[TTY_LINE_MAX + 64];
    tty_init();

    // Far more characters than a line holds: the extra ones are refused, and
    // there is still room for the newline that ends the line
    for (int i = 0; i < TTY_LINE_MAX + 100; i++) EXPECT_TRUE(!tty_input('x'));
    EXPECT_TRUE(tty_input('\n'));

    int64_t n = tty_read(buf, sizeof(buf));
    EXPECT_EQ(n, TTY_LINE_MAX);
    EXPECT_EQ(buf[TTY_LINE_MAX - 2], 'x');
    EXPECT_EQ(buf[TTY_LINE_MAX - 1], '\n');
    kprintf("\n");
}

TEST(tty_line_mode_drops_arrow_keys_and_control_characters) {
    char buf[32];
    tty_init();

    // Up arrow (ESC [ A), Delete (ESC [ 3 ~) and a stray Ctrl-G in a line
    EXPECT_TRUE(type("ab\033[Ac\033[3~d\x07" "e\n"));
    EXPECT_EQ(tty_read(buf, sizeof(buf)), 6);
    EXPECT_EQ(memcmp(buf, "abcde\n", 6), 0);

    // ESC followed by something that does not start a sequence: both dropped
    EXPECT_TRUE(type("x\033Yz\n"));
    EXPECT_EQ(tty_read(buf, sizeof(buf)), 3);
    EXPECT_EQ(memcmp(buf, "xz\n", 3), 0);

    // A tab is text
    EXPECT_TRUE(type("a\tb\n"));
    EXPECT_EQ(tty_read(buf, sizeof(buf)), 4);
    EXPECT_EQ(memcmp(buf, "a\tb\n", 4), 0);
}

TEST(tty_raw_mode_belongs_to_a_process) {
    tty_init();
    EXPECT_TRUE(!tty_is_raw());

    // Half a line typed, then a process switches to raw mode: the half line
    // is discarded, not delivered later
    type("stale");
    tty_set_raw(true, 4242);
    EXPECT_TRUE(tty_is_raw());

    // Some other process ending changes nothing; the owner ending restores
    // line mode, however it ended
    tty_process_exited(4241);
    EXPECT_TRUE(tty_is_raw());
    tty_process_exited(4242);
    EXPECT_TRUE(!tty_is_raw());

    char buf[16];
    EXPECT_TRUE(type("fresh\n"));
    EXPECT_EQ(tty_read(buf, sizeof(buf)), 6);
    EXPECT_EQ(memcmp(buf, "fresh\n", 6), 0);

    // Leaving raw mode explicitly works too
    tty_set_raw(true, 7);
    tty_set_raw(false, 7);
    EXPECT_TRUE(!tty_is_raw());
    tty_process_exited(7);
    EXPECT_TRUE(!tty_is_raw());
}

void test_suite_tty(void) {
    kprintf("Suite: Terminal Line Discipline\n");
    RUN_TEST(tty_delivers_whole_edited_lines);
    RUN_TEST(tty_short_reads_and_end_of_file);
    RUN_TEST(tty_line_length_is_bounded);
    RUN_TEST(tty_line_mode_drops_arrow_keys_and_control_characters);
    RUN_TEST(tty_raw_mode_belongs_to_a_process);
}
