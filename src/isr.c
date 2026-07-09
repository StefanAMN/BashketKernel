#include "isr.h"
#include "kprintf.h"

// Names per Intel SDM Vol. 3 Table 6-1, cross-checked against the `x86_64`
// Rust crate's InterruptDescriptorTable (OSDev Wiki itself was unreachable
// while writing this -- see the note in isr_stubs.S).
static const char *exception_names[32] = {
    "Divide Error",                    // 0
    "Debug",                           // 1
    "Non-Maskable Interrupt",          // 2
    "Breakpoint",                      // 3
    "Overflow",                        // 4
    "BOUND Range Exceeded",            // 5
    "Invalid Opcode",                  // 6
    "Device Not Available",            // 7
    "Double Fault",                    // 8
    "Coprocessor Segment Overrun",     // 9  (legacy, unused on modern CPUs)
    "Invalid TSS",                     // 10
    "Segment Not Present",             // 11
    "Stack-Segment Fault",             // 12
    "General Protection Fault",        // 13
    "Page Fault",                      // 14
    "Reserved",                        // 15
    "x87 Floating-Point Exception",    // 16
    "Alignment Check",                 // 17
    "Machine Check",                   // 18
    "SIMD Floating-Point Exception",   // 19
    "Virtualization Exception",        // 20
    "Control Protection Exception",    // 21
    "Reserved",                        // 22
    "Reserved",                        // 23
    "Reserved",                        // 24
    "Reserved",                        // 25
    "Reserved",                        // 26
    "Reserved",                        // 27
    "Hypervisor Injection Exception",  // 28
    "VMM Communication Exception",     // 29
    "Security Exception",              // 30
    "Reserved",                        // 31
};

void isr_handler(struct interrupt_frame *f) {
    kprintf("\n!!! CPU EXCEPTION %d (%s) !!!\n", (int)f->vector, exception_names[f->vector]);
    // kprintf has no 64-bit hex verb (see src/kprintf.c); %p takes a void*
    // so it prints the full 64-bit value without truncation, unlike %x
    // (which reads a 32-bit unsigned int) -- used for every register below.
    kprintf("error_code=%p\n", (void *)f->error_code);
    kprintf("rip=%p cs=%p rflags=%p\n", (void *)f->rip, (void *)f->cs, (void *)f->rflags);
    kprintf("rax=%p rbx=%p rcx=%p rdx=%p\n", (void *)f->rax, (void *)f->rbx, (void *)f->rcx, (void *)f->rdx);
    kprintf("rsi=%p rdi=%p rbp=%p\n", (void *)f->rsi, (void *)f->rdi, (void *)f->rbp);
    kprintf("r8=%p  r9=%p  r10=%p r11=%p\n", (void *)f->r8, (void *)f->r9, (void *)f->r10, (void *)f->r11);
    kprintf("r12=%p r13=%p r14=%p r15=%p\n", (void *)f->r12, (void *)f->r13, (void *)f->r14, (void *)f->r15);

    panic("Unhandled CPU exception");
}
