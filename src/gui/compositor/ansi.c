/**
 * @file ansi.c
 * @brief ANSI escape sequence parser (see ansi.h)
 */

#include "ansi.h"

#define ESC 0x1B

enum { STATE_TEXT, STATE_ESCAPE, STATE_CSI };

void ansi_init(ansi_parser_t *parser) {
    parser->state = STATE_TEXT;
    parser->param_count = 0;
    parser->digits_seen = false;
    for (int i = 0; i < ANSI_MAX_PARAMS; i++) parser->params[i] = 0;
}

static ansi_action_t nothing(void) {
    ansi_action_t action = { .kind = ANSI_NOTHING };
    return action;
}

// The sequence is complete: `final` says which command it was
static ansi_action_t finish(ansi_parser_t *parser, char final) {
    ansi_action_t action = { .kind = ANSI_NOTHING };

    // "ESC [ 5 m" has one parameter; "ESC [ m" has none; "ESC [ 5 ; m" has two
    int count = parser->param_count + (parser->digits_seen || parser->param_count > 0 ? 1 : 0);
    if (count > ANSI_MAX_PARAMS) count = ANSI_MAX_PARAMS;
    action.param_count = count;
    for (int i = 0; i < count; i++) action.params[i] = parser->params[i];

    switch (final) {
        case 'm': action.kind = ANSI_SGR; break;
        case 'H':
        case 'f': action.kind = ANSI_CURSOR_TO; break;
        case 'A': action.kind = ANSI_CURSOR_UP; break;
        case 'B': action.kind = ANSI_CURSOR_DOWN; break;
        case 'C': action.kind = ANSI_CURSOR_RIGHT; break;
        case 'D': action.kind = ANSI_CURSOR_LEFT; break;
        case 'J': action.kind = ANSI_ERASE_DISPLAY; break;
        case 'K': action.kind = ANSI_ERASE_LINE; break;
        default:  break; // A command we do not implement: swallowed
    }

    ansi_init(parser);
    return action;
}

ansi_action_t ansi_feed(ansi_parser_t *parser, char c) {
    switch (parser->state) {
        case STATE_TEXT:
            if (c == ESC) {
                parser->state = STATE_ESCAPE;
                return nothing();
            }
            return (ansi_action_t){ .kind = ANSI_PRINT, .ch = c };

        case STATE_ESCAPE:
            if (c == '[') {
                parser->state = STATE_CSI;
                parser->param_count = 0;
                parser->digits_seen = false;
                for (int i = 0; i < ANSI_MAX_PARAMS; i++) parser->params[i] = 0;
                return nothing();
            }
            // ESC followed by anything else: not a sequence we know. Drop both.
            parser->state = (c == ESC) ? STATE_ESCAPE : STATE_TEXT;
            return nothing();

        case STATE_CSI:
        default:
            if (c >= '0' && c <= '9') {
                if (parser->param_count < ANSI_MAX_PARAMS) {
                    int *value = &parser->params[parser->param_count];
                    if (*value < 10000) *value = *value * 10 + (c - '0'); // Bounded: no overflow from a long number
                }
                parser->digits_seen = true;
                return nothing();
            }
            if (c == ';') {
                if (parser->param_count < ANSI_MAX_PARAMS) parser->param_count++;
                parser->digits_seen = false;
                return nothing();
            }
            if (c >= 0x40 && c <= 0x7E) return finish(parser, c); // The final character
            if (c >= 0x20 && c <= 0x3F) return nothing();          // Other parameter/intermediate bytes ("?", " ", ...): ignored
            // A control character in the middle: give up on the sequence
            ansi_init(parser);
            return nothing();
    }
}

uint32_t ansi_color(int index) {
    static const uint32_t palette[16] = {
        0x000000, 0xAA0000, 0x00AA00, 0xAA5500, 0x0000AA, 0xAA00AA, 0x00AAAA, 0xAAAAAA,
        0x555555, 0xFF5555, 0x55FF55, 0xFFFF55, 0x5555FF, 0xFF55FF, 0x55FFFF, 0xFFFFFF,
    };
    return (index >= 0 && index < 16) ? palette[index] : palette[15];
}
