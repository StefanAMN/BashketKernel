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
// PROCESS_WUNTRACED: also return for a child that has been stopped; its
// status is then PROCESS_STATUS_STOPPED | the stop signal.
#define PROCESS_WUNTRACED 2
#define PROCESS_STATUS_STOPPED 0x10000

// fork fails rather than leave fewer free frames than this (256 KiB)
#define PROCESS_FORK_RESERVE_FRAMES 64

/**
 * @brief Wait for a child of the caller to end, or (with
 *        PROCESS_WUNTRACED) to stop. `pid` > 0 names the child, -1 means
 *        any. With PROCESS_WNOHANG, return 0 at once if none has.
 * @param status Receives its exit status (or PROCESS_STATUS_STOPPED | signal).
 * @return The child's PID; 0 (WNOHANG, nothing yet); -1 if the caller has no
 *         such child; PROCESS_WAIT_INTERRUPTED if a signal ended the wait.
 *         An ended child is collected (freed) by this call.
 */
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

/**
 * @brief Give frames to whatever part of [addr, addr + len) the calling
 *        process has been promised and has not touched yet (stack growth,
 *        lazy mmap and heap). The kernel calls this before using a buffer a
 *        program passed in: its own access must not fault, since it may be
 *        holding a lock. Addresses that were never promised are left alone
 *        (the range check that follows rejects them).
 * @return false only if memory ran out.
 */
bool process_populate(uint64_t addr, size_t len);

// --- Jobs --------------------------------------------------------------------
//
// A process group is a set of processes that a shell started for one command
// (a pipeline). One group at a time is in the foreground of the terminal:
// Ctrl-C and Ctrl-Z act on it and on nobody else.

/**
 * @brief Move process `pid` (0: the caller) into group `pgid` (0: a new group
 *        named after the process). Only the caller itself or its children.
 */
int process_set_group(pid_t pid, uint32_t pgid);

/**
 * @brief Send a signal to every process of a group that `sender_uid` may
 *        signal. -1 if that was none.
 */
int process_kill_group(uint32_t pgid, int sig, uint32_t sender_uid);

/**
 * @brief Make a group the terminal's foreground group (0: no job control).
 *        Reverts to 0 when the calling process exits.
 */
int process_set_foreground(uint32_t pgid);

/**
 * @brief The terminal's foreground process group (0: none, no job control).
 */
uint32_t process_get_foreground(void);

/**
 * @brief Process `pid` is ending: if it set the foreground group, that
 *        setting goes with it, so that a dead shell does not leave the
 *        keyboard aimed at one of its jobs.
 */
void process_foreground_owner_exited(uint32_t pid);

/**
 * @brief A signal typed at the keyboard: to the foreground group. Without
 *        one, SIGINT goes to every user process and other signals nowhere.
 * @return false if the key had no meaning and should be treated as input.
 */
bool process_signal_foreground(int sig);

/**
 * @brief Start `path` with arguments `argv` as the init process: the one
 *        that adopts orphaned processes and waits for them (see
 *        sched_set_init). Returns its PID or a negative value.
 */
pid_t process_spawn_init(const char *path, char *const argv[]);

#endif // PROCESS_H
