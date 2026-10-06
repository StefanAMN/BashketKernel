/**
 * @file ansi.h
 * @brief ANSI escape sequence parser
 *
 * Programs control a terminal by writing special byte sequences in with
 * their text: ESC [ 3 1 m switches to red, ESC [ 2 J clears the screen,
 * ESC [ 5 ; 10 H moves the cursor to row 5, column 10. This parser takes the
 * output one character at a time and says what each one amounts to: a
 * character to draw, nothing yet (the middle of a sequence), or a completed
 * command with its numbers.
 *
 * It knows nothing about screens; the console acts on what it reports. That
 * also makes it testable on its own.
 */

#ifndef ANSI_H
#define ANSI_H

#include <stdint.h>
#include <stdbool.h>

#define ANSI_MAX_PARAMS 8

typedef enum {
    ANSI_NOTHING,        // Part of a sequence: nothing to do yet (or an unsupported one, now over)
    ANSI_PRINT,          // An ordinary character: draw it
    ANSI_SGR,            // ESC [ ... m   colours and attributes, one per parameter
    ANSI_CURSOR_TO,      // ESC [ row ; col H   (also f); 1-based, default 1
    ANSI_CURSOR_UP,      // ESC [ n A     default 1
    ANSI_CURSOR_DOWN,    // ESC [ n B
    ANSI_CURSOR_RIGHT,   // ESC [ n C
    ANSI_CURSOR_LEFT,    // ESC [ n D
    ANSI_ERASE_DISPLAY,  // ESC [ n J     0: cursor to end, 1: start to cursor, 2: everything
    ANSI_ERASE_LINE,     // ESC [ n K     same, within the line
} ansi_kind_t;

typedef struct {
    ansi_kind_t kind;
    char ch;                        // For ANSI_PRINT
    int params[ANSI_MAX_PARAMS];    // Numbers given, in order; missing ones are 0
    int param_count;                // How many were given (0 if none)
} ansi_action_t;

typedef struct {
    int state;                      // 0: text, 1: seen ESC, 2: inside ESC [ ...
    int params[ANSI_MAX_PARAMS];
    int param_count;
    bool digits_seen;               // The current parameter has at least one digit
} ansi_parser_t;

void ansi_init(ansi_parser_t *parser);

/**
 * @brief Feed one character of output.
 * @return What it amounts to. Never fails: malformed and unknown sequences
 *         are swallowed up to their final character.
 */
ansi_action_t ansi_feed(ansi_parser_t *parser, char c);

/**
 * @brief The 16 standard terminal colours as 0x00RRGGBB.
 * @param index 0-7 normal (black, red, green, yellow, blue, magenta, cyan,
 *        white), 8-15 the bright versions. Out of range gives white.
 */
uint32_t ansi_color(int index);

#endif // ANSI_H
