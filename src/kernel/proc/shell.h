/**
 * @file shell.h
 * @brief The kernel's own shell: a few built-in commands, for development
 *
 * It owns the console when the kernel is started with the "shell" boot
 * option (the developer image, BashketKernel-dev.iso), and it is what the
 * scripted tests type at. The system proper boots into /bin/init and a
 * login prompt instead (see src/init/main.c).
 */

#ifndef SHELL_H
#define SHELL_H

#include <stdbool.h>
#include <stddef.h>

// One built-in command
typedef struct {
    const char *name;
    const char *args;               // What it takes, for the help text ("" for nothing)
    const char *help;               // One line saying what it does
    bool needs_arg;                 // Refused, with its usage, when given nothing
    void (*run)(const char *arg);   // `arg`: the rest of the line after the name
} shell_command_t;

/**
 * @brief Run one command line: the first word names the command, the rest
 *        is its argument. Prints a message for an unknown command.
 */
void shell_run_cmd(const char *cmdline);

/**
 * @brief The table of commands (for the tests: every one listed must run).
 * @param count Receives how many there are.
 */
const shell_command_t *shell_commands(size_t *count);

#endif // SHELL_H
