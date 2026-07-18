/*
 * @file ctx_switch.S
 * @brief Context Switch Assembly Routine
 * 
 * Saves callee-saved registers (RBX, RBP, R12-R15, RFLAGS) of the outgoing task,
 * swaps RSP to the incoming task's stack, optionally switches CR3, and restores
 * callee-saved registers to resume execution seamlessly.
 * 
 * Signature: void ctx_switch(uint64_t **old_rsp_ptr, uint64_t *new_rsp, uint64_t *new_pml4_phys)
 * Arguments (System V ABI):
 *   RDI = old_rsp_ptr
 *   RSI = new_rsp
 *   RDX = new_pml4_phys
 */

.intel_syntax noprefix
.global ctx_switch

.section .text

ctx_switch:
    // Save current task's callee-saved registers onto its stack
    pushfq
    push r15
    push r14
    push r13
    push r12
    push rbp
    push rbx

    // Save current RSP into *old_rsp_ptr (RDI)
    test rdi, rdi
    jz .skip_save_rsp
    mov [rdi], rsp

.skip_save_rsp:
    // Load incoming task's RSP (RSI)
    mov rsp, rsi

    // If new_pml4_phys (RDX) is non-null, switch CR3 page table
    test rdx, rdx
    jz .skip_cr3_switch
    mov cr3, rdx

.skip_cr3_switch:
    // Restore incoming task's callee-saved registers from its stack
    pop rbx
    pop rbp
    pop r12
    pop r13
    pop r14
    pop r15
    popfq

    // Return to incoming task's saved RIP
    ret
