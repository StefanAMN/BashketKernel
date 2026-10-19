/**
 * @file tty.h
 * @brief The terminal: line discipline between the keyboard and read()
 *
 * Programs reading the console do not get raw key presses. The terminal
 * collects a line, echoing what is typed and applying the editing keys, and
 * hands it over only when Enter is pressed ("canonical mode" in Unix terms):
 *
 *   Backspace   erase the last character
 *   Ctrl-U      erase the whole line
 *   Ctrl-D      end of file if the line is empty, otherwise deliver the line
 *               as it is, without a newline
 *   Ctrl-C      never reaches here: the input drivers turn it into an
 *               interrupt for the running programs
 */

#ifndef TTY_H
#define TTY_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define TTY_LINE_MAX 256 // Longest line, including its newline

/**
 * @brief Start in line mode with nothing typed (also how the tests reset it).
 */
void tty_init(void);

/**
 * @brief Feed one typed character through the line discipline.
 * @return true if a line (or end-of-file) is now ready to be read. A
 *         character typed while a ready line has not been read is dropped.
 */
bool tty_input(char c);

/**
 * @brief Read from the terminal. Sleeps until a whole line has been typed,
 *        then returns up to `count` bytes of it; the rest of the line is
 *        kept for the next read.
 * @return Bytes read, 0 for end-of-file (Ctrl-D on an empty line), or -EINTR
 *         if a signal handler has to run before a line was complete.
 */
int64_t tty_read(char *out, size_t count);

/**
 * @brief Switch between line mode (the default, described above) and raw
 *        mode, in which every key is delivered at once with no echo and no
 *        editing. Raw mode ends by itself when `owner_pid` exits.
 */
void tty_set_raw(bool raw, uint32_t owner_pid);

/**
 * @brief Whether the terminal is in raw mode (read without a lock: the
 *        input interrupt handlers consult it).
 */
bool tty_is_raw(void);

/**
 * @brief Called when any process ends: gives the terminal back to line mode
 *        if that process had put it in raw mode.
 */
void tty_process_exited(uint32_t pid);

/**
 * @brief Write `count` bytes of kernel memory to the screen and the serial
 *        port. Never sleeps. Returns `count`.
 */
int64_t tty_write(const char *in, size_t count);

#endif // TTY_H
