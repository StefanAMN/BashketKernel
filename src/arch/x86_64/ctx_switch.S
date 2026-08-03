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

    // Load the incoming task's address space. Skip the write when it is
    // already loaded: writing CR3 flushes the TLB even if the value is the same.
    test rdx, rdx
    jz .skip_cr3_switch
    mov rax, cr3
    cmp rax, rdx
    je .skip_cr3_switch
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

/*
 * Where a new task starts: ctx_switch returns here the first time the task
 * is chosen. The switch was made with the scheduler lock held, and the task
 * coming in must release it (sched_task_started). Then `ret` continues to
 * the address the creator put next on the stack: the task's entry function,
 * or user_frame_return for a fork child.
 *
 * The stack is aligned for the call whatever the creator left on it; RBX is
 * free to use (a new task has no register contents to preserve).
 */
.global task_start_trampoline
.extern sched_task_started
task_start_trampoline:
    mov rbx, rsp
    and rsp, -16
    call sched_task_started
    mov rsp, rbx
    xor ebx, ebx
    ret
