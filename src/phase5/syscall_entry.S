/*
 * @file syscall_entry.S
 * @brief x86_64 Syscall Assembly Entry Stub
 * 
 * Hardware behavior on `syscall`:
 * - Saves RIP to RCX
 * - Saves RFLAGS to R11
 * - Loads Kernel CS from STAR MSR
 * 
 * Target signature: uint64_t syscall_entry(void)
 */

.intel_syntax noprefix
.global syscall_entry
.extern syscall_handler

.section .text

syscall_entry:
    // Push user RIP (RCX) and user RFLAGS (R11)
    push r11
    push rcx

    // Save general purpose registers to stack to build struct registers layout
    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push r9
    push r8
    push rbp
    push rdi
    push rsi
    push rdx
    push rcx
    push rbx
    push rax

    // Pass pointer to struct registers in RDI
    mov rdi, rsp
    call syscall_handler

    // Store return value from RAX into saved RAX spot on stack
    mov [rsp], rax

    // Restore general purpose registers
    pop rax
    pop rbx
    pop rcx
    pop rdx
    pop rsi
    pop rdi
    pop rbp
    pop r8
    pop r9
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15

    // Restore RCX (User RIP) and R11 (User RFLAGS)
    pop rcx
    pop r11

    // Return to Ring 3 via sysretq
    sysretq
