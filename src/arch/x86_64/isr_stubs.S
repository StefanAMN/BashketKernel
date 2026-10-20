.intel_syntax noprefix

// Some exceptions push an error code, some do not.
// For those that do not, we push a dummy '0' to keep the stack uniform.
.macro ISR_NOERRCODE num
.global isr\num
isr\num:
    push 0          // Dummy error code
    push \num       // Interrupt number
    jmp common_isr_stub
.endm

.macro ISR_ERRCODE num
.global isr\num
isr\num:
    // Error code is already pushed by CPU
    push \num       // Interrupt number
    jmp common_isr_stub
.endm

// CPU Exceptions (0-31)
ISR_NOERRCODE 0
ISR_NOERRCODE 1
ISR_NOERRCODE 2
ISR_NOERRCODE 3
ISR_NOERRCODE 4
ISR_NOERRCODE 5
ISR_NOERRCODE 6
ISR_NOERRCODE 7
ISR_ERRCODE   8
ISR_NOERRCODE 9
ISR_ERRCODE   10
ISR_ERRCODE   11
ISR_ERRCODE   12
ISR_ERRCODE   13
ISR_ERRCODE   14
ISR_NOERRCODE 15
ISR_NOERRCODE 16
ISR_ERRCODE   17
ISR_NOERRCODE 18
ISR_NOERRCODE 19
ISR_NOERRCODE 20
ISR_ERRCODE   21
ISR_NOERRCODE 22
ISR_NOERRCODE 23
ISR_NOERRCODE 24
ISR_NOERRCODE 25
ISR_NOERRCODE 26
ISR_NOERRCODE 27
ISR_NOERRCODE 28
ISR_ERRCODE   29
ISR_ERRCODE   30
ISR_NOERRCODE 31

// IRQs and remaining vectors (32-255). None of these push error codes.
.altmacro
.macro MAKE_ISR_NOERRCODE i
    ISR_NOERRCODE \i
.endm

.set i, 32
.rept 224
    MAKE_ISR_NOERRCODE %i
    .set i, i + 1
.endr

// The common ISR stub
.extern isr_handler
.global common_isr_stub
common_isr_stub:
    // The C ABI requires the direction flag to be clear on function entry,
    // and compiled code relies on it for rep movs/stos. The interrupted code
    // (a user program, for instance) may have set it. iretq restores the
    // interrupted flags, so this needs no undoing.
    cld

    // If the interrupt arrived in ring 3, GS still has the user's base:
    // switch to this CPU's (see percpu.h). The saved CS, whose low two bits
    // are the privilege level that was interrupted, is at [rsp + 24] here:
    // above the interrupt number, the error code and RIP.
    test qword ptr [rsp + 24], 3
    jz 1f
    swapgs
1:
    // Push all general purpose registers
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

    // Call the C handler. 
    // According to System V AMD64 ABI, the first argument goes in RDI.
    // Our stack pointer (RSP) currently points to the top of our `struct registers`.
    mov rdi, rsp
    call isr_handler

    // Pop all general purpose registers
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

    // Remove interrupt number and error code from stack
    add rsp, 16

    // Going back to ring 3: give GS back to the user. (The frame decides,
    // not how we came in: it may have been rewritten in between.)
    test qword ptr [rsp + 8], 3
    jz 2f
    swapgs
2:
    // Return from interrupt
    iretq

// We need an array of pointers to these ISRs so we can load them into the IDT easily.
.altmacro
.macro MAKE_ISR_TABLE_ENTRY i
    .quad isr\i
.endm

.global isr_stub_table
.section .data
isr_stub_table:
.set i, 0
.rept 256
    MAKE_ISR_TABLE_ENTRY %i
    .set i, i + 1
.endr

// The NMI (vector 2). It can arrive anywhere, including the few
// instructions where the CPU is in ring 0 with the user's GS base (the
// start of syscall_entry before its swapgs, the end of the return path
// after its swapgs): the saved CS cannot say whether to swapgs. The GS base
// itself can. A program's is always 0 (nothing lets one change it: see
// percpu.h); this CPU's structure is a kernel address, with the top bit
// set. So: swap if the base is not a kernel address, and swap back after.
// It runs on its own stack (IST2), whatever the interrupted stack was.
.section .text
.extern nmi_handler
.global nmi_stub
nmi_stub:
    push 0                      // Error code (none)
    push 2                      // Vector
    cld
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

    mov ecx, 0xC0000101         // IA32_GS_BASE
    rdmsr                       // EDX: its upper half
    xor ebx, ebx                // RBX (kept across the call): 1 if we swapped
    test edx, edx
    js 1f                       // Top bit set: a kernel address, GS is ours already
    swapgs
    mov ebx, 1
1:
    mov rdi, rsp
    call nmi_handler
    test ebx, ebx
    jz 2f
    swapgs
2:
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
    add rsp, 16
    iretq

// No executable stack: without this note the linker assumes this object
// needs one (and binutils older than 2.45 warns, failing the CI build)
.section .note.GNU-stack,"",@progbits
