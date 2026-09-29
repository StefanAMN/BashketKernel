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
 * A process can hold signals back (block them): a blocked signal stays
 * pending until it is unblocked. While a handler runs, its own signal is
 * blocked, plus whatever the program asked for; the previous mask comes back
 * when the handler returns. SIGKILL can be neither caught nor blocked.
 *
 * A handler installed with SA_RESTART makes the interrupted system call run
 * again after the handler instead of failing with -EINTR. Only calls that
 * have done nothing yet can be: read, write, waitpid, accept.
 */

#ifndef SIGNAL_H
#define SIGNAL_H

#include <stdint.h>
#include <stdbool.h>
#include "interrupts.h"
#include "task.h"

#define SIGINT   2   // Ctrl-C
#define SIGILL   4   // An instruction the CPU does not know (from an exception only)
#define SIGTRAP  5   // A debug exception (from an exception only)
#define SIGFPE   8   // Division by zero, or a floating point error (from an exception only)
#define SIGKILL  9   // Cannot be caught or ignored
#define SIGUSR1 10
#define SIGBUS   7  // A shared mapping's page past the end of its file
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGTERM 15
#define SIGCHLD 17   // A child of this process has exited or stopped; ignored unless a handler is set
#define SIGCONT 18   // Continue if stopped
#define SIGSTOP 19   // Stop (suspend) the process; cannot be caught, ignored or blocked
#define SIGTSTP 20   // Stop, asked for from the terminal (Ctrl-Z); a program may catch or ignore it

#define SIG_DFL 0ULL // Terminate the process
#define SIG_IGN 1ULL // Do nothing

#define SA_RESTART 1 // Restart the system call the signal interrupted

// sigprocmask: how the given set changes the set of blocked signals
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

// What a program passes to and gets from sigaction
struct signal_action {
    uint64_t handler;    // SIG_DFL, SIG_IGN or a function's address
    uint32_t mask;       // Signals (bit n: signal n) to block while the handler runs, besides its own
    uint32_t flags;      // SA_RESTART or 0
};

/**
 * @brief Set what a signal does in the calling process.
 * @param handler SIG_DFL, SIG_IGN, or the address of a handler function.
 * @param restorer Address the handler returns to; must call sigreturn.
 * @return The previous handler value, or a negative error number.
 */
int64_t signal_set_handler(task_t *task, int sig, uint64_t handler, uint64_t restorer);

/**
 * @brief The same with a mask and flags; the previous action goes to *old
 *        if that is not NULL.
 */
int signal_set_action(task_t *task, int sig, const struct signal_action *action, uint64_t restorer, struct signal_action *old);

/**
 * @brief Change the set of blocked signals of the current process.
 * @param how SIG_BLOCK, SIG_UNBLOCK or SIG_SETMASK.
 * @return The previous set through *old (if not NULL); 0 or -EINVAL.
 */
int signal_change_mask(task_t *task, int how, uint32_t set, uint32_t *old);

/**
 * @brief Whether a signal is waiting that the task is not holding back.
 */
bool signal_deliverable(const task_t *task);


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
