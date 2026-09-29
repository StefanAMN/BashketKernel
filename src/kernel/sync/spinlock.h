/**
 * @file spinlock.h
 * @brief Spinlocks, the lock order, and the run-time check of both
 *
 * A spinlock protects a data structure against use by two CPUs at once, and
 * (because it is always taken with interrupts off) against an interrupt
 * handler on the same CPU. The holder must not sleep: use
 * sched_sleep(channel, &lock, ...) to wait for something, which gives the
 * lock up while asleep.
 *
 * Rules, all checked at run time (see lock_debug_* below):
 *   1. Interrupts are off whenever a lock is held. Use the _irqsave forms.
 *   2. Locks are taken in increasing rank order. A lock may be taken while
 *      holding another only if its rank is higher. This is what makes
 *      deadlock between two CPUs impossible.
 *   3. A CPU never takes a lock it already holds.
 *   4. Only the scheduler lock is held across a task switch.
 */

#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <stdint.h>
#include <stdbool.h>

// The lock order. Lower ranks are taken first ("outer" locks); a higher rank
// may be taken while holding a lower one, never the reverse. Rank 0 means
// the lock takes no part in the ordering check (test fixtures).
enum {
    LOCK_RANK_NONE = 0,
    LOCK_RANK_FS       = 10, // File system trees and volumes
    LOCK_RANK_NET      = 20, // Network stack tables and sockets
    LOCK_RANK_IPC      = 30, // Shared memory segments
    LOCK_RANK_PIPE     = 40,
    LOCK_RANK_TTY      = 50,
    LOCK_RANK_INPUT_WAIT = 54, // "Is there any input?", across both buffers below
    LOCK_RANK_INPUT    = 55, // Keyboard and serial receive buffers
    LOCK_RANK_SEM      = 58,
    LOCK_RANK_MUTEX    = 59, // The spinlock inside a mutex_t
    LOCK_RANK_DEVICE   = 60, // Driver state: virtqueues and request slots
    LOCK_RANK_BCACHE   = 61, // The cache of disk sectors
    LOCK_RANK_HEAP     = 70, // kmalloc and the slab caches
    LOCK_RANK_KSTACK   = 72,
    LOCK_RANK_PCACHE   = 73, // The page cache's table (takes frame sharing counts under it)
    LOCK_RANK_VMM      = 75,
    LOCK_RANK_COW      = 78,
    LOCK_RANK_SWAP     = 79, // Which swap slots are in use
    LOCK_RANK_PMM      = 80,
    LOCK_RANK_POLL     = 85, // "Something may be ready": taken by notifiers under any of the above
    LOCK_RANK_SCHED    = 90, // Task list and task states
    LOCK_RANK_RUNQUEUE = 91, // A CPU's queue of runnable tasks (another CPU's only by spinlock_try_acquire)
    LOCK_RANK_FD_SLOTS = 92, // Emptying a descriptor slot, against /proc reading another process's (under the task list)
    LOCK_RANK_TIME     = 93,
    LOCK_RANK_KPRINTF  = 100, // Output: may be used while holding anything above
    LOCK_RANK_KLOG     = 101,
    LOCK_RANK_CONSOLE  = 102,
};

typedef struct spinlock {
    volatile uint32_t locked;
    uint32_t rank;         // Place in the lock order (LOCK_RANK_*)
    const char *name;      // For reports
    void *owner;           // The cpu_t holding it, while locked
    volatile uint64_t contended; // Acquires that found it held and had to wait (sched_lock_contention)
} spinlock_t;

#define SPINLOCK_INIT ((spinlock_t){0})
#define SPINLOCK_RANKED(lock_name, lock_rank) ((spinlock_t){ 0, (lock_rank), (lock_name), 0, 0 })

static inline void spinlock_init(spinlock_t *lock) {
    lock->locked = 0;
    lock->rank = LOCK_RANK_NONE;
    lock->name = 0;
    lock->owner = 0;
    lock->contended = 0;
}

static inline void spinlock_init_ranked(spinlock_t *lock, const char *name, uint32_t rank) {
    lock->locked = 0;
    lock->rank = rank;
    lock->name = name;
    lock->owner = 0;
    lock->contended = 0;
}

/**
 * @brief Take the lock, waiting (spinning) while another CPU holds it.
 *        Interrupts must already be off.
 */
void spinlock_acquire(spinlock_t *lock);

/**
 * @brief Take the lock if nobody holds it, without waiting. Interrupts
 *        must be off. The lock order is not checked: a try never waits,
 *        so it cannot be half of a deadlock (two CPUs taking each other's
 *        run queue lock rely on this). Returns whether it was taken.
 */
bool spinlock_try_acquire(spinlock_t *lock);

/**
 * @brief Release a lock taken with spinlock_acquire (interrupts stay as
 *        they are).
 */
void spinlock_release(spinlock_t *lock);

/**
 * @brief Turn interrupts off and take the lock. Returns the interrupt state
 *        to hand back to spinlock_release_irqrestore.
 */
uint64_t spinlock_acquire_irqsave(spinlock_t *lock);

/**
 * @brief Release the lock and put interrupts back as they were when
 *        spinlock_acquire_irqsave returned `flags`.
 */
void spinlock_release_irqrestore(spinlock_t *lock, uint64_t flags);

/**
 * @brief Clear a lock whoever holds it. Only for fatal paths that must
 *        print (panic, exception reports).
 */
void spinlock_force_release(spinlock_t *lock);

/**
 * @brief Whether the calling CPU holds this lock.
 */
bool spinlock_held(spinlock_t *lock);

// Turn interrupts off on this CPU and say whether they were on. This keeps
// interrupt handlers out; it does NOT keep other CPUs out. Use it only for
// state that belongs to this CPU.
static inline uint64_t irq_save(void) {
    uint64_t flags;
    asm volatile("pushfq\n\tpop %0\n\tcli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void irq_restore(uint64_t flags) {
    if (flags & 0x200) { // RFLAGS.IF
        asm volatile("sti");
    }
}

// --- Run-time check of the rules above ---------------------------------------

/**
 * @brief Start checking. Called once per-CPU data exists.
 */
void lock_debug_enable(void);

/**
 * @brief Number of rule violations seen since boot.
 */
uint32_t lock_debug_violations(void);

/**
 * @brief Number of locks the calling CPU holds now.
 */
uint32_t lock_debug_held_count(void);

/**
 * @brief Record a violation found by other code (the scheduler reports a
 *        task switch with locks held through this).
 */
void lock_debug_report(const char *what, const char *lock_a, const char *lock_b);

/**
 * @brief Forget recorded violations. For the tests that break a rule on
 *        purpose to see it noticed.
 */
void lock_debug_reset(void);

/**
 * @brief While `expected` is true, lock_debug_poll reports nothing: a test
 *        is breaking a rule on purpose and will reset the count itself.
 *        Every CPU's timer polls, so without this another CPU (or this one,
 *        where the test must have interrupts on) can halt the kernel in
 *        the moment between a deliberate violation and its reset.
 */
void lock_debug_expect(bool expected);

/**
 * @brief If a violation has been recorded, describe it and stop the kernel.
 *        Called from the timer interrupt, where no lock is held and
 *        printing is safe.
 */
void lock_debug_poll(void);

#endif // SPINLOCK_H
