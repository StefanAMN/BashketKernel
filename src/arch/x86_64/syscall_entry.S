/*
 * @file syscall_entry.S
 * @brief Entry and exit paths between ring 3 and the kernel
 *
 * What the CPU does on `syscall` (Intel SDM Vol. 2B, SYSCALL):
 *   RCX    <- RIP of the next user instruction
 *   R11    <- RFLAGS
 *   RFLAGS <- RFLAGS & ~SFMASK   (we mask IF, DF and AC)
 *   CS/SS  <- kernel selectors from STAR[47:32]
 *   RIP    <- LSTAR
 * It does NOT switch stacks: RSP is still the user's. The first job here is
 * to get onto a kernel stack before touching memory.
 *
 * The frame built below is laid out exactly like `struct registers`
 * (interrupts.h), the same layout the interrupt stubs build. That is what
 * lets fork() copy a frame and resume the child through user_frame_return.
 */

.intel_syntax noprefix
.global syscall_entry
.global user_frame_return
.global enter_user_mode
.extern syscall_handler

.section .text

// Offsets into this CPU's cpu_t, reached through GS (see percpu.h)
.set CPU_KERNEL_RSP, 8
.set CPU_USER_RSP, 16

syscall_entry:
    // A system call always comes from ring 3, so GS has the user's base:
    // switch to this CPU's. Then park the user's stack pointer in the per-CPU
    // scratch slot and move to this CPU's kernel stack. Interrupts are off
    // (SFMASK), so nothing can use the slot in between.
    swapgs
    mov gs:[CPU_USER_RSP], rsp
    mov rsp, gs:[CPU_KERNEL_RSP]

    // Build the part of struct registers that the CPU pushes on an interrupt
    push 0x1B                               // SS  (user data, RPL 3)
    push qword ptr gs:[CPU_USER_RSP]        // RSP (user)
    push r11                                // RFLAGS (user)
    push 0x23                               // CS  (user code, RPL 3)
    push rcx                                // RIP (user)
    push 0                                  // err_code
    push 0                                  // int_no

    // General purpose registers, same order as common_isr_stub
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

    // Now on a private kernel stack with a complete frame: safe to be preempted
    sti

    mov rdi, rsp
    call syscall_handler

    cli

    // sigreturn replaces the whole frame with the state a signal interrupted,
    // which may be any point in the program, so every register has to come
    // back exactly. sysretq cannot do that (it uses RCX and R11), so such a
    // frame is marked with a non-zero int_no and restored with iretq.
    cmp qword ptr [rsp + 15*8], 0
    jne user_frame_return

    mov [rsp + 14*8], rax       // Return value goes into the saved RAX slot

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
    add rsp, 16                 // int_no, err_code

    // sysretq takes RIP from RCX and RFLAGS from R11, and sets CS/SS from
    // STAR[63:48]. syscall_handler has already refused to return to a
    // non-canonical RIP: on Intel CPUs that would fault here in ring 0, after
    // RSP has been switched back to the user's stack.
    pop rcx                     // RIP
    add rsp, 8                  // CS (sysretq supplies it)
    pop r11                     // RFLAGS
    pop rsp                     // User RSP. SS slot is abandoned with the stack.
    swapgs                      // GS back to the user's base
    sysretq

/*
 * void user_frame_return(void)
 *
 * Entered with RSP pointing at a complete struct registers. Restores it and
 * drops to ring 3 with iretq. Used for a task's first entry to user mode
 * after fork() (the frame is a copy of the parent's syscall frame) and for
 * the return from sigreturn.
 */
user_frame_return:
    cli
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
    test qword ptr [rsp + 8], 3 // Returning to ring 3 (it always is, but let the frame decide)
    jz 1f
    swapgs
1:
    iretq

/*
 * void enter_user_mode(uint64_t rip, uint64_t rsp)
 *
 * First entry to ring 3 for a new program. iretq is the general way to change
 * privilege level: it pops RIP, CS, RFLAGS, RSP, SS, and because CS has RPL 3
 * the CPU switches to ring 3.
 */
enter_user_mode:
    cli
    push 0x1B                   // SS
    push rsi                    // RSP
    push 0x202                  // RFLAGS: IF set, reserved bit 1 set
    push 0x23                   // CS
    push rdi                    // RIP

    // Do not leak kernel register contents into the new program
    xor eax, eax
    xor ebx, ebx
    xor ecx, ecx
    xor edx, edx
    xor esi, esi
    xor edi, edi
    xor ebp, ebp
    xor r8d, r8d
    xor r9d, r9d
    xor r10d, r10d
    xor r11d, r11d
    xor r12d, r12d
    xor r13d, r13d
    xor r14d, r14d
    xor r15d, r15d
    swapgs                      // Leaving for ring 3: GS back to the user's base
    iretq

// No executable stack: without this note the linker assumes this object
// needs one (and binutils older than 2.45 warns, failing the CI build)
.section .note.GNU-stack,"",@progbits
