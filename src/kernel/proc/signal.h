/**
 * @file signal.h
 * @brief Signals: asynchronous notifications to a process
 *
 * Each signal has a disposition per process: terminate the process (the
 * default), ignore it, or call a handler function in the program.
 *
 * Running a handler means making the program call a function it did not ask
 * to call, at an arbitrary point, and then carry on as if nothing happened.
 * The kernel does that when the process is about to return to user mode:
 *
 *   1. It saves the interrupted state (all registers, FPU included) in a
 *      "signal frame" on the program's own stack.
 *   2. It rewrites the return so that the program continues in the handler,
 *      with a return address pointing at a tiny routine in the C library.
 *   3. That routine makes the sigreturn system call, and the kernel puts the
 *      saved state back.
 *
 * The frame lives in user memory, so the program can change it. sigreturn
 * therefore treats it as untrusted input and never restores anything that
 * would give the program more privilege than it had.
 *
 * A process asleep in a system call (reading the terminal or a pipe, waiting
 * for a child, sleeping) is woken for a signal: to be ended, or, if the
 * signal has a handler, the call returns -EINTR and the handler runs on the
 * way back to user mode.
 *
 * Limits: no signal mask, and an interrupted call is never restarted
 * automatically; the program has to call it again.
 */

#ifndef SIGNAL_H
#define SIGNAL_H

#include <stdint.h>
#include <stdbool.h>
#include "interrupts.h"
#include "task.h"

#define SIGINT   2   // Ctrl-C
#define SIGKILL  9   // Cannot be caught or ignored
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGTERM 15

#define SIG_DFL 0ULL // Terminate the process
#define SIG_IGN 1ULL // Do nothing

/**
 * @brief Set what a signal does in the calling process.
 * @param handler SIG_DFL, SIG_IGN, or the address of a handler function.
 * @param restorer Address the handler returns to; must call sigreturn.
 * @return The previous handler value, or a negative error number.
 */
int64_t signal_set_handler(task_t *task, int sig, uint64_t handler, uint64_t restorer);

/**
 * @brief Send a signal to a process. Safe to call from interrupt handlers.
 */
void signal_send(task_t *task, int sig);

// The same, for a caller that already holds the scheduler lock
void signal_send_locked(task_t *task, int sig);

/**
 * @brief Safe point: if the current process has a signal waiting for a
 *        handler, arrange for the return to user mode described by `regs` to
 *        enter the handler instead. Call only when regs is about to be used
 *        to return to ring 3.
 */
void signal_deliver(struct registers *regs);

/**
 * @brief The sigreturn system call: put back the state saved by
 *        signal_deliver. Rewrites *regs; ends the process if the frame on its
 *        stack is unusable.
 */
void signal_return(struct registers *regs);

/**
 * @brief exec: handlers point into the old program, so they go back to the
 *        default. Ignored signals stay ignored.
 */
void signal_reset_on_exec(task_t *task);

#endif // SIGNAL_H
