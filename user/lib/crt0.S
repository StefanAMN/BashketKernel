/*
 * crt0: the first user-mode code of every program.
 *
 * The kernel enters here with RSP pointing at argc, followed by the argv
 * pointers and a NULL (System V AMD64 process entry layout). This passes them
 * to main() and turns its return value into an exit() system call.
 */
.intel_syntax noprefix
.global _start
.extern main
.extern exit

.section .text
_start:
    xor ebp, ebp            // Mark the outermost stack frame
    mov rdi, [rsp]          // argc
    lea rsi, [rsp + 8]      // argv
    call main               // RSP is 16-byte aligned here, as the ABI requires
    mov edi, eax
    call exit
    ud2                     // exit() does not return
