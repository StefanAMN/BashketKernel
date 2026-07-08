#include "kprintf.h"
#include "serial.h"
#include "console.h"
#include <stdarg.h>
#include <stdint.h>

// A simple function to print an unsigned integer in a given base
static void print_uint(uint64_t val, int base) {
    char buf[32];
    int i = 0;
    
    if (val == 0) {
        serial_putchar('0');
        console_putchar('0');
        return;
    }
    
    while (val > 0) {
        int rem = val % base;
        buf[i++] = (rem < 10) ? (rem + '0') : (rem - 10 + 'a');
        val /= base;
    }
    
    while (i > 0) {
        serial_putchar(buf[i - 1]);
        console_putchar(buf[i - 1]);
        i--;
    }
}

// A simple function to print a signed integer
static void print_int(int64_t val) {
    if (val < 0) {
        serial_putchar('-');
        console_putchar('-');
        val = -val;
    }
    print_uint(val, 10);
}

void kprintf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    
    for (const char* p = fmt; *p != '\0'; p++) {
        if (*p != '%') {
            serial_putchar(*p);
            console_putchar(*p);
            continue;
        }
        
        p++; // Skip '%'
        switch (*p) {
            case 'd':
                print_int(va_arg(args, int));
                break;
            case 'u':
                print_uint(va_arg(args, unsigned int), 10);
                break;
            case 'x':
                print_uint(va_arg(args, unsigned int), 16);
                break;
            case 'p':
                serial_write("0x");
                console_write("0x");
                print_uint((uint64_t)va_arg(args, void*), 16);
                break;
            case 's': {
                const char* s = va_arg(args, const char*);
                if (!s) s = "(null)";
                serial_write(s);
                console_write(s);
                break;
            }
            case 'c': {
                char c = (char)va_arg(args, int);
                serial_putchar(c);
                console_putchar(c);
                break;
            }
            case '%':
                serial_putchar('%');
                console_putchar('%');
                break;
            default:
                serial_putchar('%');
                console_putchar('%');
                serial_putchar(*p);
                console_putchar(*p);
                break;
        }
    }
    
    va_end(args);
}

void panic(const char* msg) {
    kprintf("\n\n!!! KERNEL PANIC !!!\n");
    kprintf("%s\n", msg);
    kprintf("System halted.\n");
    asm volatile("cli");
    while (1) {
        asm volatile("hlt");
    }
}
