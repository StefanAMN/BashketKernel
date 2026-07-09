.intel_syntax noprefix
.text

// Generates one ISR entry point per exception vector. GAS expands each
// macro invocation into a labeled function, so this is really only ~10
// lines of logic even though it produces 32 distinct symbols below.
//
// Two variants exist because the CPU only pushes a real error code for
// some exceptions (Intel SDM Vol. 3 Table 6-1 / cross-checked against the
// widely-used `x86_64` Rust crate's InterruptDescriptorTable layout, since
// the OSDev Wiki itself wasn't reachable while writing this). To keep the
// frame layout uniform for every vector, ISR_NOERR pushes a dummy 0 in the
// error code's place.
.macro ISR_NOERR num
.global isr\num
isr\num:
    push 0
    push \num
    jmp isr_common_stub
.endm

.macro ISR_ERR num
.global isr\num
isr\num:
    push \num
    jmp isr_common_stub
.endm

ISR_NOERR 0   // #DE Divide Error
ISR_NOERR 1   // #DB Debug
ISR_NOERR 2   //     NMI
ISR_NOERR 3   // #BP Breakpoint
ISR_NOERR 4   // #OF Overflow
ISR_NOERR 5   // #BR BOUND Range Exceeded
ISR_NOERR 6   // #UD Invalid Opcode
ISR_NOERR 7   // #NM Device Not Available
ISR_ERR   8   // #DF Double Fault (error code always 0)
ISR_NOERR 9   //     Coprocessor Segment Overrun (legacy, unused)
ISR_ERR   10  // #TS Invalid TSS
ISR_ERR   11  // #NP Segment Not Present
ISR_ERR   12  // #SS Stack-Segment Fault
ISR_ERR   13  // #GP General Protection Fault
ISR_ERR   14  // #PF Page Fault
ISR_NOERR 15  //     Reserved
ISR_NOERR 16  // #MF x87 Floating-Point Exception
ISR_ERR   17  // #AC Alignment Check
ISR_NOERR 18  // #MC Machine Check
ISR_NOERR 19  // #XM SIMD Floating-Point Exception
ISR_NOERR 20  // #VE Virtualization Exception
ISR_ERR   21  // #CP Control Protection Exception
ISR_NOERR 22  //     Reserved
ISR_NOERR 23  //     Reserved
ISR_NOERR 24  //     Reserved
ISR_NOERR 25  //     Reserved
ISR_NOERR 26  //     Reserved
ISR_NOERR 27  //     Reserved
ISR_NOERR 28  // #HV Hypervisor Injection Exception
ISR_ERR   29  // #VC VMM Communication Exception
ISR_ERR   30  // #SX Security Exception
ISR_NOERR 31  //     Reserved

// Common trampoline: save the rest of the register state, hand the C
// handler a pointer to the whole frame, then (structurally) restore and
// return. isr_handler() currently always calls panic() and never returns,
// so the pop/iretq tail below is unreachable in this slice -- it's kept so
// the stub is already correct for a future slice where a handler might
// resume execution (e.g. a recoverable page fault).
//
// pusha/popa do not exist in 64-bit mode (SDM marks them invalid), so each
// GPR is pushed individually. No ring transition happens anywhere in this
// slice (there is no usermode yet), so the CPU frame is only
// RFLAGS/CS/RIP (+ error code) -- not the 5-word ring3->ring0 frame most
// tutorials lead with.
isr_common_stub:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    // rsp now points at the low (last-pushed) end of struct interrupt_frame
    // -- see src/isr.h, whose field order is the exact reverse of the
    // pushes above.
    mov rdi, rsp
    call isr_handler

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16   // discard vector + error_code
    iretq

.section .rodata
.global isr_stub_table
isr_stub_table:
    .quad isr0,  isr1,  isr2,  isr3,  isr4,  isr5,  isr6,  isr7
    .quad isr8,  isr9,  isr10, isr11, isr12, isr13, isr14, isr15
    .quad isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23
    .quad isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31
