/**
 * @file process.h
 * @brief User process creation and lifecycle
 */

#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "interrupts.h"

#define SIGINT  2
#define SIGKILL 9
#define SIGSEGV 11

// User address space layout (see also mmap.h)
#define USER_STACK_TOP   0x00007FFFFFFFF000ULL
#define USER_STACK_PAGES 32                     // 128 KiB
#define PROCESS_MAX_ARGS 32
#define PROCESS_ARG_BYTES 2048                  // Total size of all argv strings

typedef int pid_t;

/**
 * @brief Start a new process running the ELF executable at `path`.
 * @param argv NULL-terminated argument vector (kernel memory), or NULL for
 *        just the program name.
 * @return PID of the new process, or -1 if the file is missing, not a valid
 *         executable, or memory ran out.
 */
pid_t process_spawn(const char *path, char *const argv[]);

/**
 * @brief Replace the calling process's program with the one at `path`.
 * @param regs The caller's syscall frame; rewritten to start the new program.
 * @param argv NULL-terminated argument vector in kernel memory.
 * @return 0 on success (the frame now belongs to the new program), -1 on
 *         failure (the caller's program is untouched).
 */
int process_exec(struct registers *regs, const char *path, char *const argv[]);

/**
 * @brief Create a copy of the calling process.
 * @param regs The caller's syscall frame; the child resumes from a copy of it.
 * @return Child PID to the parent; the child sees 0 from its own syscall return.
 */
pid_t process_fork(struct registers *regs);

/**
 * @brief Wait for a child to exit and collect its status.
 * @param pid Child to wait for, or -1 for any child.
 * @param status If non-NULL, receives the exit status.
 * @return PID of the child collected, or -1 if there is no such child.
 */
pid_t process_wait(pid_t pid, int *status);

/**
 * @brief Terminate a process as if by a signal.
 * @return 0 on success, -1 if there is no such user process.
 */
int process_kill(pid_t pid, int sig);

#endif // PROCESS_H
