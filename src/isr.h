#ifndef ISR_H
#define ISR_H

#include <stdint.h>

// Field order is the exact reverse of the push order in
// isr_common_stub (src/isr_stubs.S): the last register pushed (r15) ends
// up at the lowest address, i.e. this struct's first field.
struct __attribute__((packed)) interrupt_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector;
    uint64_t error_code;
    // CPU-pushed. No rsp/ss here: those are only pushed on a privilege
    // change (ring transition), and nothing runs outside ring 0 yet.
    uint64_t rip, cs, rflags;
};

// Called from isr_common_stub. Currently always fatal (calls panic()).
void isr_handler(struct interrupt_frame *frame);

#endif // ISR_H
