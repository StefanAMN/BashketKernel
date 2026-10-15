/**
 * @file task.h
 * @brief Task Control Block (TCB) and Process State Definitions
 * 
 * Defines task structures for kernel threads and userspace processes.
 */

#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "region.h"

#define KERNEL_STACK_SIZE (16 * 1024) // 16 KiB kernel stack per task
#define TASK_MAX_FDS 16               // Open file descriptors per process
#define TASK_NSIG 32                  // Signal numbers 1..31

struct file;
struct vnode;

// Priorities. Every runnable task gets a turn (round robin), so none can be
// starved; a higher priority gets a longer turn.
#define PRIO_HIGH   0
#define PRIO_NORMAL 1
#define PRIO_LOW    2

#define TASK_ANY_CPU (-1)

// PID of the idle tasks, which are not on the task list
#define IDLE_PID 0xFFFFFFFFu

typedef enum {
    TASK_READY,
    TASK_RUNNING,
    TASK_BLOCKED,
    TASK_ZOMBIE,
    TASK_STOPPED            // Suspended by a stop signal until SIGCONT (or SIGKILL)
} task_state_t;

typedef struct task {
    uint64_t rsp;            // Saved kernel stack pointer (points to saved registers frame)
    uint32_t pid;            // Process ID
    char name[32];           // Task description name
    task_state_t state;      // Current execution state
    uint64_t *pml4_phys;     // Physical address of PML4, loaded into CR3 when this task runs
    void *kernel_stack;      // Virtual base pointer of 16 KiB kernel stack
    struct task *next;       // Next task link in scheduler queue

    // --- CPU time ---
    uint8_t priority;        // PRIO_HIGH, PRIO_NORMAL or PRIO_LOW: sets the time slice length
    uint8_t slice_left;      // Timer ticks left before it is preempted
    uint64_t cpu_ticks;      // Timer ticks that found this task running
    int32_t affinity;        // The only CPU it may run on, or TASK_ANY_CPU
    uint32_t last_cpu;       // The CPU it is running on, or last ran on

    // --- Blocking (valid while state is TASK_BLOCKED) ---
    void *wait_channel;      // What it is waiting for; sched_wakeup(channel) wakes it
    uint64_t wake_ns;        // The same, as a time on the clock_ns scale (one or the other is set)
    uint64_t wake_tick;      // If non-zero, timer tick at which it wakes regardless

    // --- Process state (user tasks only) ---
    bool is_user;            // Owns a user address space and runs in ring 3
    bool detached;           // Nobody will wait for it: the idle loop reaps it on exit
    uint32_t parent_pid;     // PID of the task that created it
    int exit_status;         // Valid once state is TASK_ZOMBIE
    uint64_t user_entry;     // Ring 3 entry point for a freshly created process
    uint64_t user_rsp;       // Initial ring 3 stack pointer
    uint64_t brk_start;      // First address after the loaded program image
    uint64_t brk_end;        // Current program break (heap end)
    uint64_t mmap_next;      // Next free address in the mmap region
    mem_region_t regions[TASK_MAX_REGIONS]; // Memory promised by mmap, given page by page on first touch (region.h)
    size_t swap_retry_below; // After a search that found nothing: do not search again until fewer frames than this are free (0: no such search yet)
    uint64_t swap_cursor;    // The clock hand: where the last search for a page to swap out stopped (swap.h)
    size_t promised_pages;   // Pages of those regions, and of the heap, not yet touched
    uint64_t stack_top;      // Highest address of the user stack (randomised per program)
    struct file *fds[TASK_MAX_FDS]; // Open files; the index is the file descriptor
    uint32_t fd_cloexec;     // Bit n set: descriptor n is closed when the process execs a new program
    void *io_buffer;         // One page for read() and write() to copy through (allocated on first use)
    uint32_t uid;            // Effective user: whose rights this process has (0: root, may do anything)
    uint32_t ruid;           // Real user: who started it (differs from uid only in a set-uid program)
    uint32_t gid;            // Its group
    uint16_t umask;          // Permission bits removed from every file it creates
    struct vnode *cwd;       // Working directory for relative paths (NULL: the root)
    uint32_t pgid;           // Process group: the job it belongs to (signals from the terminal go to a whole group)
    int stop_pending;        // Stop signal that will suspend this process at its next safe point (0: none)
    int stop_signal;         // While TASK_STOPPED: the signal that did it
    bool stop_reported;      // ...and whether waitpid has told the parent yet
    bool exiting;            // Has begun to exit: can no longer be killed or signalled
    int kill_pending;        // Signal that will end this process at its next safe point (0: none)
    uint64_t sig_handlers[TASK_NSIG]; // Per signal: 0 default (terminate), 1 ignore, else handler address
    uint64_t sig_restorer;   // Where handlers return to: the C library's sigreturn stub
    uint32_t sig_pending;    // Bit n set: signal n has arrived and not yet been acted on
    uint32_t sig_blocked;    // Bit n set: signal n is held back (stays pending) until unblocked
    uint32_t sig_masks[TASK_NSIG]; // Per signal: what else to block while its handler runs
    uint8_t sig_flags[TASK_NSIG];  // Per signal: SA_RESTART
    uint32_t restart_syscall;      // Number + 1 of the system call a signal has just cut short, if it can be made again

    // FPU/SSE registers while the task is not running (FXSAVE format).
    // FXSAVE needs 16-byte alignment: kmalloc guarantees it for the struct,
    // the attribute for this member within it.
    uint8_t fpu_state[512] __attribute__((aligned(16)));
} task_t;

#endif // TASK_H
