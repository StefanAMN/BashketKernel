/**
 * @file timer.h
 * @brief The timer interrupt: what time it is in ticks, and who is woken when
 *
 * The kernel counts time in ticks of 10 ms (TIMER_HZ in pit.h) and can keep
 * that count in two ways.
 *
 * Periodic: the PIT interrupts the boot CPU 100 times a second and each
 * interrupt adds one. Simple, and how the kernel starts. But a sleep can only
 * end on a tick, and a machine with nothing to do is still interrupted 100
 * times a second (on every CPU: the others have periodic timers of their own
 * for the scheduler).
 *
 * One shot ("tickless"): nothing interrupts regularly. The tick count is
 * read off the timestamp counter, and each CPU's local APIC timer is set to
 * fire once, at the next moment that CPU has something to do:
 *
 *   - while it runs a task, 10 ms from now, to share the CPU as before;
 *   - at the earliest time any task that went to sleep on it asked to be
 *     woken, to the microsecond rather than to the tick;
 *   - when the screen has output that has not been shown yet;
 *   - otherwise never: an idle CPU sleeps until a device or another CPU
 *     interrupts it.
 *
 * The kernel switches to the second way during boot if the CPU has a
 * timestamp counter and the local APIC timer can be calibrated, and stays
 * with the first if not.
 */

#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/**
 * @brief Ticks since boot. Advances in both modes; does not need interrupts
 *        in one-shot mode.
 */
uint64_t timer_ticks(void);

/**
 * @brief Count one tick (the PIT's interrupt handler, periodic mode).
 */
void timer_periodic_tick(void);

/**
 * @brief Change from the periodic tick to one-shot timers. Called once on
 *        the boot CPU, after the clock is calibrated and before the other
 *        CPUs schedule. Returns false, changing nothing, if this machine
 *        can not do it.
 */
bool timer_start_one_shot(void);

bool timer_is_one_shot(void);

/**
 * @brief Start the calling CPU's own timer, in whichever mode is in use.
 *        (Other CPUs than the boot CPU, as they join the scheduler.)
 */
void timer_start_on_this_cpu(void);

/**
 * @brief The local APIC timer's interrupt, on any CPU.
 */
void timer_interrupt(void);

/**
 * @brief The moment (clock_ns scale) at which timer_ticks() reaches `tick`.
 *        One-shot mode only.
 */
uint64_t timer_tick_to_ns(uint64_t tick);

// The three calls below are the scheduler's, made with interrupts off on the
// CPU concerned. All do nothing in periodic mode.

/**
 * @brief A task is going to sleep on this CPU until `deadline_ns`: see that
 *        the timer fires by then.
 */
void timer_sleeper_deadline(uint64_t deadline_ns);

/**
 * @brief This CPU is leaving its idle task to run a real one: it needs the
 *        10 ms interrupt that shares the CPU out.
 */
void timer_cpu_busy(void);

/**
 * @brief This CPU is about to halt with nothing to run: set the timer for
 *        the next thing it must wake for, or switch it off.
 */
void timer_cpu_idle(void);

/**
 * @brief Timer interrupts this CPU has taken in one-shot mode.
 */
uint64_t timer_interrupt_count(uint32_t cpu_id);

/**
 * @brief Text for /proc/timers.
 */
size_t timer_describe(char *buf, size_t size);

#endif // TIMER_H
