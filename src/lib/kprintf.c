#include "kprintf.h"
#include "serial.h"
#include "console.h"
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "spinlock.h"
#include "klog.h"

// Keeps output from two tasks (or a task and an interrupt handler) from
// interleaving mid-line. This is a leaf lock: nothing is acquired under it.
static spinlock_t kprintf_lock = SPINLOCK_INIT;

void kprintf_force_unlock(void) {
    spinlock_release(&kprintf_lock);
}

// The formatter writes through a "sink", so the same code serves kprintf
// (characters go to the serial port and the screen) and ksnprintf (characters
// go into a buffer).
typedef struct {
    void (*put)(void *ctx, char c);
    void *ctx;
} sink_t;

static void emit_string(sink_t *out, const char *s) {
    while (*s) out->put(out->ctx, *s++);
}

// Print a number right-aligned in a field of at least `width` characters,
// padded with spaces or (zero_pad) zeros. A minus sign goes before the zeros.
static void emit_number(sink_t *out, uint64_t val, unsigned base, bool negative, int width, bool zero_pad) {
    char buf[32];
    int len = 0;

    do {
        unsigned rem = (unsigned)(val % base);
        buf[len++] = (char)((rem < 10) ? (rem + '0') : (rem - 10 + 'a'));
        val /= base;
    } while (val > 0);

    int padding = width - len - (negative ? 1 : 0);
    if (!zero_pad) while (padding-- > 0) out->put(out->ctx, ' ');
    if (negative) out->put(out->ctx, '-');
    if (zero_pad) while (padding-- > 0) out->put(out->ctx, '0');
    while (len > 0) out->put(out->ctx, buf[--len]);
}

static void emit_int(sink_t *out, int64_t val, int width, bool zero_pad) {
    // Negate as unsigned: INT64_MIN has no positive counterpart
    uint64_t magnitude = val < 0 ? 0 - (uint64_t)val : (uint64_t)val;
    emit_number(out, magnitude, 10, val < 0, width, zero_pad);
}

// Supports %d %u %x %p %s %c %%.
//   A width may come first (%5d, %8s), with a leading 0 for zero padding
//   (%02d, %08x). Fields are right-aligned.
//   A 'z' or 'l' before d/u/x means a 64-bit argument (size_t, long).
static void format(sink_t *out, const char *fmt, va_list args) {
    for (const char *p = fmt; *p != '\0'; p++) {
        if (*p != '%') {
            out->put(out->ctx, *p);
            continue;
        }

        p++; // Skip '%'

        bool zero_pad = (*p == '0');
        if (zero_pad) p++;
        int width = 0;
        while (*p >= '0' && *p <= '9') {
            if (width < 64) width = width * 10 + (*p - '0'); // Bounded: no runaway padding
            p++;
        }

        bool is_long = false;
        while (*p == 'z' || *p == 'l') {
            is_long = true;
            p++;
        }

        switch (*p) {
            case 'd':
                emit_int(out, is_long ? va_arg(args, int64_t) : va_arg(args, int), width, zero_pad);
                break;
            case 'u':
                emit_number(out, is_long ? va_arg(args, uint64_t) : va_arg(args, unsigned int), 10, false, width, zero_pad);
                break;
            case 'x':
                emit_number(out, is_long ? va_arg(args, uint64_t) : va_arg(args, unsigned int), 16, false, width, zero_pad);
                break;
            case 'p':
                emit_string(out, "0x");
                emit_number(out, (uint64_t)va_arg(args, void *), 16, false, 0, false);
                break;
            case 's': {
                const char *str = va_arg(args, const char *);
                if (!str) str = "(null)";
                int len = 0;
                while (str[len]) len++;
                for (int pad = width - len; pad > 0; pad--) out->put(out->ctx, ' ');
                emit_string(out, str);
                break;
            }
            case 'c':
                out->put(out->ctx, (char)va_arg(args, int));
                break;
            case '%':
                out->put(out->ctx, '%');
                break;
            case '\0':
                return; // Format ended in the middle of a specifier
            default:
                out->put(out->ctx, '%');
                out->put(out->ctx, *p);
                break;
        }
    }
}

static void put_to_console(void *ctx, char c) {
    (void)ctx;
    serial_putchar(c);
    console_putchar(c);
    klog_putchar(c); // Kept for dmesg and /proc/kmsg
}

void kprintf(const char* fmt, ...) {
    sink_t out = { put_to_console, NULL };
    va_list args;
    va_start(args, fmt);

    uint64_t flags = spinlock_acquire_irqsave(&kprintf_lock);
    format(&out, fmt, args);
    spinlock_release_irqrestore(&kprintf_lock, flags);

    va_end(args);
}

typedef struct {
    char *buf;
    size_t size;
    size_t len; // Characters the full output needs so far
} buffer_sink_t;

static void put_to_buffer(void *ctx, char c) {
    buffer_sink_t *b = ctx;
    if (b->len + 1 < b->size) b->buf[b->len] = c;
    b->len++;
}

size_t ksnprintf(char *buf, size_t size, const char *fmt, ...) {
    buffer_sink_t target = { buf, size, 0 };
    sink_t out = { put_to_buffer, &target };

    va_list args;
    va_start(args, fmt);
    format(&out, fmt, args);
    va_end(args);

    if (size > 0) buf[target.len < size ? target.len : size - 1] = '\0';
    return target.len < size ? target.len : (size ? size - 1 : 0);
}

void panic(const char* msg) {
    asm volatile("cli");
    kprintf_force_unlock();
    kprintf("\n\n!!! KERNEL PANIC !!!\n");
    kprintf("%s\n", msg);
    kprintf("System halted.\n");
    console_refresh(); // The timer will not do it: interrupts stay off
    while (1) {
        asm volatile("cli; hlt");
    }
}
