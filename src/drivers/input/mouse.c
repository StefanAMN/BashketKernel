/**
 * @file mouse.c
 * @brief PS/2 mouse driver (see mouse.h)
 */

#include "mouse.h"
#include "keyboard.h"
#include "interrupts.h"
#include "ioapic.h"
#include "spinlock.h"
#include "sched.h"
#include "random.h"
#include "kprintf.h"
#include "kerrno.h"


#define MOUSE_IRQ 12

// Commands a mouse understands (each is answered with ACK)
#define MOUSE_SET_DEFAULTS     0xF6
#define MOUSE_ENABLE_REPORTING 0xF4

#define PACKET_ALWAYS_ONE 0x08
#define PACKET_X_SIGN     0x10
#define PACKET_Y_SIGN     0x20
#define PACKET_OVERFLOW   0xC0

// Movements waiting to be read. Each entry is "the buttons became this,
// and the mouse moved this far": the distance may be more than one event's
// worth, and is then read out in several.
#define QUEUE_SIZE 64
static struct {
    int32_t dx, dy;
    uint8_t buttons;
} queue[QUEUE_SIZE];
static unsigned queue_head, queue_count;    // Oldest entry, and how many

static uint8_t packet[3];
static unsigned packet_have;                // Bytes of the current packet received so far
static uint8_t last_buttons;
static bool present;

// Protects all of the above. Readers sleep on `queue`.
static spinlock_t mouse_lock = SPINLOCK_RANKED("mouse", LOCK_RANK_INPUT);

// Caller holds mouse_lock
static void push_event(int32_t dx, int32_t dy, uint8_t buttons) {
    if (queue_count > 0) {
        // Movement with the buttons unchanged adds to the newest entry: a
        // program that reads slowly sees one long movement, not a backlog.
        // So does everything when the queue is full, so that the total
        // distance, and the final state of the buttons, are never lost.
        unsigned newest = (queue_head + queue_count - 1) % QUEUE_SIZE;
        if (queue[newest].buttons == buttons || queue_count == QUEUE_SIZE) {
            queue[newest].dx += dx;
            queue[newest].dy += dy;
            queue[newest].buttons = buttons;
            return;
        }
    }
    unsigned at = (queue_head + queue_count) % QUEUE_SIZE;
    queue[at].dx = dx;
    queue[at].dy = dy;
    queue[at].buttons = buttons;
    queue_count++;
}

void mouse_handle_byte(uint8_t byte) {
    uint64_t flags = spinlock_acquire_irqsave(&mouse_lock);

    // A first byte without the always-one bit is not a first byte: we are
    // out of step with the mouse (a byte was lost). Drop bytes until one
    // could be the start of a packet.
    if (packet_have == 0 && !(byte & PACKET_ALWAYS_ONE)) {
        spinlock_release_irqrestore(&mouse_lock, flags);
        return;
    }
    packet[packet_have++] = byte;
    if (packet_have < 3) {
        spinlock_release_irqrestore(&mouse_lock, flags);
        return;
    }
    packet_have = 0;

    uint8_t buttons = packet[0] & (MOUSE_LEFT | MOUSE_RIGHT | MOUSE_MIDDLE);
    // The movements are 9-bit two's complement numbers: eight bits in their
    // own byte and the sign in the first
    int32_t dx = (int32_t)packet[1] - ((packet[0] & PACKET_X_SIGN) ? 256 : 0);
    int32_t dy = (int32_t)packet[2] - ((packet[0] & PACKET_Y_SIGN) ? 256 : 0);
    if (packet[0] & PACKET_OVERFLOW) dx = dy = 0;   // The numbers are meaningless; the buttons are not

    if (dx != 0 || dy != 0 || buttons != last_buttons) {
        push_event(dx, dy, buttons);
        last_buttons = buttons;
        sched_wakeup(queue);
    }
    spinlock_release_irqrestore(&mouse_lock, flags);
}

int64_t mouse_read(void *buf, size_t count, bool block) {
    uint8_t *out = buf;
    size_t done = 0;
    if (count < MOUSE_EVENT_SIZE) return -EINVAL;

    uint64_t flags = spinlock_acquire_irqsave(&mouse_lock);
    while (queue_count == 0 && block) {
        if (sched_interrupted()) {
            spinlock_release_irqrestore(&mouse_lock, flags);
            return -EINTR;
        }
        sched_sleep(queue, &mouse_lock, 0);
    }

    while (queue_count > 0 && count - done >= MOUSE_EVENT_SIZE) {
        // As much of the oldest entry's movement as one event can carry
        int32_t dx = queue[queue_head].dx, dy = queue[queue_head].dy;
        int32_t step_x = dx > 127 ? 127 : dx < -127 ? -127 : dx;
        int32_t step_y = dy > 127 ? 127 : dy < -127 ? -127 : dy;
        out[done++] = (uint8_t)(queue[queue_head].buttons | PACKET_ALWAYS_ONE);
        out[done++] = (uint8_t)(int8_t)step_x;
        out[done++] = (uint8_t)(int8_t)step_y;
        queue[queue_head].dx -= step_x;
        queue[queue_head].dy -= step_y;
        if (queue[queue_head].dx == 0 && queue[queue_head].dy == 0) {
            queue_head = (queue_head + 1) % QUEUE_SIZE;
            queue_count--;
        }
    }
    spinlock_release_irqrestore(&mouse_lock, flags);
    return (int64_t)done;
}

void mouse_flush(void) {
    uint64_t flags = spinlock_acquire_irqsave(&mouse_lock);
    queue_head = queue_count = 0;
    packet_have = 0;
    last_buttons = 0;
    spinlock_release_irqrestore(&mouse_lock, flags);
}

bool mouse_present(void) {
    return present;
}

// IRQ 12: feed every byte the controller holds for the mouse to the decoder
static void mouse_irq(struct registers *regs) {
    // The byte is fetched through the keyboard driver, which shares the
    // controller's one data port (and decides whose byte it is)
    uint8_t byte;
    while (ps2_read_aux(&byte)) {
        random_add_event(((uint64_t)byte << 32) ^ (regs ? regs->rip : 0));
        mouse_handle_byte(byte);
    }
}

int mouse_init(void) {
    if (present) return 0;
    if (!ps2_aux_enable() || !ps2_aux_command(MOUSE_SET_DEFAULTS) || !ps2_aux_command(MOUSE_ENABLE_REPORTING)) {
        kprintf("mouse: no PS/2 mouse answered\n");
        return -1;
    }
    register_irq_handler(MOUSE_IRQ, mouse_irq);
    ioapic_route_isa_irq(MOUSE_IRQ);
    present = true;
    kprintf("mouse: PS/2 mouse ready, IRQ %u\n", MOUSE_IRQ);
    return 0;
}
