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
#include "signal.h"


// User address space layout (see also mmap.h)
#define USER_STACK_TOP   0x00007FFFFFFFF000ULL  // Highest possible stack top; the real one is up to 16 MiB lower
#define USER_STACK_PAGES 4                      // Mapped at start (16 KiB); the rest on demand
#define USER_STACK_MAX   (8ULL * 1024 * 1024)   // The stack may grow down to TOP - MAX
#define PROCESS_MAX_ARGS 32
#define PROCESS_MAX_IMAGE_BYTES (16 * 1024 * 1024) // Largest executable loaded from disk
#define PROCESS_ARG_BYTES 4096                  // Total size of all argument and environment strings

typedef int pid_t;

#define PROCESS_WAIT_INTERRUPTED (-2)

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
 * @param envp NULL-terminated environment ("NAME=value" strings), or NULL
 *        for an empty one. The environment itself lives in user memory; the
 *        kernel only carries it from the old image to the new one.
 * @return 0 on success (the frame now belongs to the new program), -1 on
 *         failure (the caller's program is untouched).
 */
int process_exec(struct registers *regs, const char *path, char *const argv[], char *const envp[]);

/**
 * @brief Create a copy of the calling process. Memory is shared copy-on-write.
 * @param regs The caller's syscall frame; the child resumes from a copy of it.
 * @return Child PID to the parent; the child sees 0 from its own syscall return.
 */
pid_t process_fork(struct registers *regs);

/**
 * @brief Demand paging for the stack: if `addr` lies in the calling process's
 *        stack region and has no page yet, give it a zeroed one.
 * @return true if a page was mapped and the faulting access can be retried.
 */
bool process_grow_stack(uint64_t addr);

/**
 * @brief Wait for a child to exit and collect its status.
 * @param pid Child to wait for, or -1 for any child.
 * @param status If non-NULL, receives the exit status.
 * @return PID of the child collected, -1 if there is no such child, or
 *         PROCESS_WAIT_INTERRUPTED if a signal handler has to run first.
 */
pid_t process_wait(pid_t pid, int *status);

// The same, with options. PROCESS_WNOHANG: do not wait; return 0 if there
// are children to wait for but none has exited yet.
#define PROCESS_WNOHANG 1

// fork fails rather than leave fewer free frames than this (256 KiB)
#define PROCESS_FORK_RESERVE_FRAMES 64
pid_t process_wait_options(pid_t pid, int *status, int options);

/**
 * @brief Send a signal to a process. What happens depends on the target's
 *        disposition for it (see signal.h); with the default, the process
 *        ends at its next safe point, so wait for it to see that.
 * @return 0 on success, -1 if there is no such user process or signal.
 */
int process_kill(pid_t pid, int sig);

/**
 * @brief Ctrl-C: send SIGINT to every user process. Safe to call from an
 *        interrupt handler.
 */
void process_interrupt_all(void);

#endif // PROCESS_H
