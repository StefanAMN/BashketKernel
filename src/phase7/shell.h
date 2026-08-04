/**
 * @file shell.h
 * @brief Interactive Kernel Shell Interface
 */

#ifndef SHELL_H
#define SHELL_H

/**
 * @brief Initialize Interactive Shell subsystem.
 */
void shell_init(void);

/**
 * @brief Execute a shell command string.
 * @param cmdline Command line string (e.g. "ls", "help", "cat file.txt").
 */
void shell_run_cmd(const char *cmdline);

#endif // SHELL_H
