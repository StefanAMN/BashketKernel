/**
 * @file power.c
 * @brief Shutting the system down (see power.h)
 */

#include "power.h"
#include "acpi.h"
#include "ext2.h"
#include "sched.h"
#include "signal.h"
#include "console.h"
#include "kprintf.h"
#include "timer.h"
#include "pit.h"

// Send `sig` to every user process except the caller. Returns how many are
// still alive (not yet zombies), after the signal.
static unsigned signal_others(int sig) {
    task_t *self = sched_get_current_task();
    unsigned alive = 0;
    uint64_t flags = sched_lock_irqsave();
    for (task_t *t = sched_first_task(); t; t = t->next) {
        if (!t->is_user || t == self || t->state == TASK_ZOMBIE) continue;
        if (sig) signal_send_locked(t, sig);
        alive++;
    }
    sched_unlock_irqrestore(flags);
    return alive;
}

// Wait until no other user process is alive, or `ticks` have passed.
// Returns how many are still alive.
static unsigned wait_for_others(uint64_t ticks) {
    uint64_t deadline = timer_ticks() + ticks;
    unsigned alive;
    while ((alive = signal_others(0)) > 0 && timer_ticks() < deadline) sched_sleep_ticks(5);
    return alive;
}

void system_shutdown(bool restart) {
    // Begun, it is finished: whoever called it (a root process) must not be
    // killable by the others it is about to end, or the machine is left
    // with every process told to stop and nothing switching it off
    sched_make_unkillable(sched_get_current_task());
    kprintf("\nThe system is going down for %s.\n", restart ? "a restart" : "power-off");

    // Politely first: a process may catch SIGTERM and finish what it is
    // doing. A second later, not politely.
    if (signal_others(SIGTERM) > 0 && wait_for_others(TIMER_HZ) > 0) {
        signal_others(SIGKILL);
        unsigned left = wait_for_others(2 * TIMER_HZ);
        if (left > 0) kprintf("shutdown: %u processes did not end\n", left);
    }

    if (ext2_mounted()) {
        ext2_mark_clean();
        kprintf("ext2: volume marked clean\n");
    }

    if (restart) {
        kprintf("Restarting.\n");
        console_refresh();
        acpi_reset();
    } else {
        kprintf("Switching off.\n");
        console_refresh();
        acpi_power_off();
    }

    // Still here: the firmware did not do it
    kprintf("It is now safe to switch the machine off.\n");
    kernel_halt_other_cpus();
    console_refresh();
    for (;;) asm volatile("cli\n\thlt");
}
