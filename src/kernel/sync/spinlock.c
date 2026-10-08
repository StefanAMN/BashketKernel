/**
 * @file spinlock.c
 * @brief Spinlocks and the run-time check of the locking rules (see spinlock.h)
 */

#include "spinlock.h"
#include "percpu.h"
#include "kprintf.h"
#include "smp.h"

// Off until per-CPU data exists: the check finds the calling CPU through GS
static bool tracking = false;

static volatile uint32_t violation_count = 0;
static struct {
    const char *what;
    const char *lock_a;
    const char *lock_b;
    uint32_t cpu_id;
} first_violation;

static const char *name_of(const spinlock_t *lock) {
    return lock && lock->name ? lock->name : "(unnamed)";
}

void lock_debug_enable(void) {
    tracking = true;
}

uint32_t lock_debug_violations(void) {
    return violation_count;
}

uint32_t lock_debug_held_count(void) {
    return tracking ? this_cpu()->held_count : 0;
}

void lock_debug_report(const char *what, const char *lock_a, const char *lock_b) {
    // Keep the first one: later ones are usually consequences of it
    if (__atomic_fetch_add(&violation_count, 1, __ATOMIC_SEQ_CST) == 0) {
        first_violation.what = what;
        first_violation.lock_a = lock_a;
        first_violation.lock_b = lock_b;
        first_violation.cpu_id = tracking ? this_cpu()->cpu_id : 0;
    }
}

void lock_debug_reset(void) {
    violation_count = 0;
}

void lock_debug_poll(void) {
    if (violation_count == 0) return;
    kprintf_force_unlock();
    kprintf("\nLOCK RULE VIOLATION on CPU %u: %s: %s%s%s (%u in total)\n",
            first_violation.cpu_id, first_violation.what,
            first_violation.lock_a ? first_violation.lock_a : "",
            first_violation.lock_b ? ", while holding " : "",
            first_violation.lock_b ? first_violation.lock_b : "",
            violation_count);
    panic("locking rules violated");
}

static inline bool interrupts_enabled(void) {
    uint64_t flags;
    asm volatile("pushfq\n\tpop %0" : "=r"(flags) : : "memory");
    return (flags & 0x200) != 0;
}

bool spinlock_held(spinlock_t *lock) {
    return tracking && lock->locked && lock->owner == this_cpu();
}

void spinlock_acquire(spinlock_t *lock) {
    cpu_t *cpu = tracking ? this_cpu() : NULL;

    if (cpu) {
        if (interrupts_enabled()) lock_debug_report("taken with interrupts enabled", name_of(lock), NULL);

        // Taking a lock this CPU already holds would spin for ever. There is
        // no carrying on from that, so report it now.
        if (lock->locked && lock->owner == cpu) {
            asm volatile("cli");
            kprintf_force_unlock();
            kprintf("\nDEADLOCK: CPU %u takes lock '%s', which it already holds\n", cpu->cpu_id, name_of(lock));
            panic("recursive spinlock");
        }

        if (lock->rank != LOCK_RANK_NONE) {
            for (uint32_t i = 0; i < cpu->held_count; i++) {
                spinlock_t *held = cpu->held[i];
                if (held->rank != LOCK_RANK_NONE && held->rank >= lock->rank) {
                    lock_debug_report("taken out of order", name_of(lock), name_of(held));
                    break;
                }
            }
        }
    }

    while (__atomic_test_and_set(&lock->locked, __ATOMIC_ACQUIRE)) {
        // The CPU we are waiting for may itself be waiting for us to drop
        // stale page-table lookups (see smp_tlb_shootdown)
        if (cpu) smp_tlb_service();
        asm volatile("pause");
    }

    if (cpu) {
        lock->owner = cpu;
        if (cpu->held_count < LOCK_HELD_MAX) cpu->held[cpu->held_count++] = lock;
        else lock_debug_report("too many locks held at once", name_of(lock), NULL);
    }
}

// Locks are usually released in the reverse of the order they were taken,
// but not always (going to sleep releases the caller's lock while holding
// the scheduler's), so search rather than pop
static void forget_held(cpu_t *cpu, spinlock_t *lock) {
    for (uint32_t i = cpu->held_count; i > 0; i--) {
        if (cpu->held[i - 1] != lock) continue;
        for (uint32_t j = i; j < cpu->held_count; j++) cpu->held[j - 1] = cpu->held[j];
        cpu->held_count--;
        return;
    }
}

void spinlock_release(spinlock_t *lock) {
    if (tracking) {
        cpu_t *cpu = this_cpu();
        if (!lock->locked || lock->owner != cpu) lock_debug_report("released by a CPU that does not hold it", name_of(lock), NULL);
        forget_held(cpu, lock);
        lock->owner = NULL;
    }
    __atomic_clear(&lock->locked, __ATOMIC_RELEASE);
}

void spinlock_force_release(spinlock_t *lock) {
    if (tracking) {
        forget_held(this_cpu(), lock);
        lock->owner = NULL;
    }
    __atomic_clear(&lock->locked, __ATOMIC_RELEASE);
}

uint64_t spinlock_acquire_irqsave(spinlock_t *lock) {
    uint64_t flags = irq_save();
    spinlock_acquire(lock);
    return flags;
}

void spinlock_release_irqrestore(spinlock_t *lock, uint64_t flags) {
    spinlock_release(lock);
    irq_restore(flags);
}
