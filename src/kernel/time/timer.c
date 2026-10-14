/**
 * @file timer.c
 * @brief The timer interrupt, periodic or one shot (see timer.h)
 */

#include "timer.h"
#include "pit.h"
#include "hrtimer.h"
#include "lapic.h"
#include "smp.h"
#include "percpu.h"
#include "sched.h"
#include "console.h"
#include "spinlock.h"
#include "io.h"
#include "kprintf.h"

#define NS_PER_TICK (1000000000ULL / TIMER_HZ)
#define REFRESH_NS  (2 * NS_PER_TICK)       // Show output at most 50 times a second (see console.c)

static volatile uint64_t counted_ticks = 0; // Periodic mode: one per PIT interrupt
static bool one_shot = false;

// One-shot mode: ticks = clock / NS_PER_TICK + tick_offset, chosen so that
// the count carries on from where the PIT left it
static uint64_t tick_offset = 0;

// What each CPU's timer is doing. Only ever touched by its own CPU, with
// interrupts off (the counters are read by others, for display).
static struct cpu_timer {
    uint64_t armed_ns;      // When the timer is set to fire (0: it is not set)
    uint64_t next_wake_ns;  // No later than the earliest wake-up time of the tasks asleep on this CPU (0: none)
    uint64_t slice_due_ns;  // When the running task's 10 ms are up
    uint64_t interrupts;
} timers[MAX_CPUS];

static uint64_t last_refresh_ns = 0;        // Shared: any CPU may be the one to refresh the screen

bool timer_is_one_shot(void) {
    return one_shot;
}

uint64_t timer_ticks(void) {
    if (!one_shot) return counted_ticks;
    return hrtimer_raw_ns() / NS_PER_TICK + tick_offset;
}

uint64_t timer_tick_to_ns(uint64_t tick) {
    return (tick - tick_offset) * NS_PER_TICK;
}

uint64_t timer_interrupt_count(uint32_t cpu_id) {
    return cpu_id < MAX_CPUS ? timers[cpu_id].interrupts : 0;
}

bool sched_sleep_ticks(uint64_t count) {
    if (count == 0) return true;

    // No lock: the wake-up time is part of the sleep itself, so a deadline
    // that passes just before we sleep still wakes us
    uint64_t deadline = timer_ticks() + count;
    while (timer_ticks() < deadline) {
        if (sched_interrupted()) return false;
        sched_sleep(NULL, NULL, deadline);
    }
    return true;
}

// --- Periodic mode --------------------------------------------------------------

void timer_periodic_tick(void) {
    if (one_shot) return; // A last interrupt from the PIT, already on its way when it was stopped
    uint64_t now = ++counted_ticks;

    // Nothing is locked here, so this is where a recorded breach of the
    // locking rules can be reported
    lock_debug_poll();

    if (now % 2 == 0) console_refresh();

    // Wake tasks whose sleep has ended, then let the scheduler preempt
    sched_wake_sleepers(now, 0);
    sched_tick();
}

// --- One-shot mode --------------------------------------------------------------

static uint64_t earlier(uint64_t a, uint64_t b) { // Of two times, where 0 means "none"
    if (a == 0) return b;
    if (b == 0) return a;
    return a < b ? a : b;
}

// Set this CPU's timer to fire at `at` (at once if that is past)
static void arm(struct cpu_timer *t, uint64_t at, uint64_t now) {
    uint64_t per_tick = lapic_timer_counts_per_tick();

    // The counter is 32 bits wide: a time too far off is approached in
    // steps. (Firing early is harmless: whoever is woken looks at the
    // clock, finds nothing due, and the timer is set again.)
    uint64_t longest = (0xFFFFFFFFULL / per_tick) * NS_PER_TICK;
    uint64_t wait = at > now ? at - now : 0;
    if (wait > longest) wait = longest;

    uint64_t counts = wait * per_tick / NS_PER_TICK + 1; // Rounded up: never early by a fraction of a count
    if (counts > 0xFFFFFFFFULL) counts = 0xFFFFFFFFULL;
    lapic_timer_arm((uint32_t)counts);
    t->armed_ns = at;
}

// The screen is refreshed by whichever CPU's timer finds it stale, but not
// more often than REFRESH_NS: copying the whole picture is slow
static void refresh_console_if_due(uint64_t now) {
    if (!console_needs_refresh()) return;
    uint64_t last = __atomic_load_n(&last_refresh_ns, __ATOMIC_RELAXED);
    if (now - last < REFRESH_NS) return;
    if (__atomic_compare_exchange_n(&last_refresh_ns, &last, now, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
        console_refresh();
    }
}

void timer_interrupt(void) {
    if (!one_shot) {      // A CPU's periodic timer: its scheduler tick
        sched_tick();
        return;
    }

    cpu_t *cpu = this_cpu();
    struct cpu_timer *t = &timers[cpu->cpu_id];
    uint64_t now = hrtimer_raw_ns();
    t->interrupts++;
    t->armed_ns = 0;      // It has fired, and fires once

    lock_debug_poll();
    refresh_console_if_due(now);

    if (t->next_wake_ns != 0 && now >= t->next_wake_ns) {
        t->next_wake_ns = sched_wake_sleepers(timer_ticks(), now);
    }

    // A CPU that is running a task is interrupted every 10 ms as it always
    // was. One that is idle sets its timer on the way back to sleep (see
    // timer_cpu_idle), when it knows that it is still idle.
    bool idle = cpu->current == cpu->idle;
    bool slice_over = !idle && now >= t->slice_due_ns;
    if (slice_over) t->slice_due_ns = now + NS_PER_TICK;
    if (!idle) arm(t, earlier(t->slice_due_ns, t->next_wake_ns), now);

    // Last, because it may switch to another task and not come back here
    // for a long time
    if (idle || slice_over) sched_tick();
}

void timer_sleeper_deadline(uint64_t deadline_ns) {
    if (!one_shot || deadline_ns == 0) return;
    struct cpu_timer *t = &timers[this_cpu()->cpu_id];
    t->next_wake_ns = earlier(t->next_wake_ns, deadline_ns);
    if (t->armed_ns == 0 || deadline_ns < t->armed_ns) arm(t, deadline_ns, hrtimer_raw_ns());
}

void timer_cpu_busy(void) {
    if (!one_shot) return;
    struct cpu_timer *t = &timers[this_cpu()->cpu_id];
    uint64_t now = hrtimer_raw_ns();
    t->slice_due_ns = now + NS_PER_TICK;
    arm(t, earlier(t->slice_due_ns, t->next_wake_ns), now);
}

void timer_cpu_idle(void) {
    if (!one_shot) return;
    struct cpu_timer *t = &timers[this_cpu()->cpu_id];

    uint64_t at = t->next_wake_ns;
    if (console_needs_refresh()) {
        at = earlier(at, __atomic_load_n(&last_refresh_ns, __ATOMIC_RELAXED) + REFRESH_NS);
    }
    if (at == 0) {
        // Nothing to wake for. Whatever the timer was set to no longer
        // matters: switch it off.
        if (t->armed_ns != 0) {
            lapic_timer_arm(0);
            t->armed_ns = 0;
        }
        return;
    }
    if (t->armed_ns != at) arm(t, at, hrtimer_raw_ns());
}

void timer_start_on_this_cpu(void) {
    if (one_shot) {
        // Not set to anything: this CPU is idle, and its idle loop decides
        lapic_timer_one_shot_on_this_cpu(IPI_VECTOR_TIMER);
    } else {
        lapic_timer_start_on_this_cpu(IPI_VECTOR_TIMER);
    }
}

bool timer_start_one_shot(void) {
    if (one_shot) return true;
    if (hrtimer_tsc_hz() == 0) {
        kprintf("timer: no usable timestamp counter; staying with the 100 Hz tick\n");
        return false;
    }
    if (lapic_timer_counts_per_tick() == 0 && !lapic_timer_calibrate(timer_ticks, 5)) {
        kprintf("timer: the local APIC timer does not count; staying with the 100 Hz tick\n");
        return false;
    }

    uint64_t flags = irq_save();
    struct cpu_timer *t = &timers[this_cpu()->cpu_id];
    uint64_t now = hrtimer_raw_ns();
    tick_offset = counted_ticks - now / NS_PER_TICK;
    one_shot = true;

    // Stop the PIT. It can not be switched off, but in mode 0 (command byte
    // 0x30: channel 0, both bytes, "interrupt on terminal count") it counts
    // down once, raises its output and leaves it there: no more edges.
    outb(0x43, 0x30);
    outb(0x40, 0xFF);
    outb(0x40, 0xFF);

    lapic_timer_one_shot_on_this_cpu(IPI_VECTOR_TIMER);

    // Tasks already asleep were to be woken by the tick: look at them at once
    t->next_wake_ns = now;
    t->slice_due_ns = now + NS_PER_TICK;
    arm(t, now, now);
    irq_restore(flags);

    kprintf("timer: one-shot local APIC timers (%u counts per 10 ms); an idle CPU is not interrupted\n",
            lapic_timer_counts_per_tick());
    return true;
}

size_t timer_describe(char *buf, size_t size) {
    size_t len = ksnprintf(buf, size, "mode: %s\nticks: %lu\n", one_shot ? "one-shot" : "periodic", timer_ticks());
    for (uint32_t i = 0; i < MAX_CPUS && len < size; i++) {
        if (!cpu_get(i)->online) continue;
        len += ksnprintf(buf + len, size - len, "cpu%u: %lu timer interrupts\n", i, timers[i].interrupts);
    }
    return len < size ? len : size;
}
