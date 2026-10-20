/*
 * crt0: the first user-mode code of every program.
 *
 * The kernel enters here with RSP pointing at argc, followed by the argv
 * pointers, a NULL, the environment pointers and another NULL (System V AMD64
 * process entry layout). This passes them to main(argc, argv, envp), records
 * the environment for getenv(), and turns main's return value into an exit()
 * system call.
 */
.intel_syntax noprefix
.global _start
.extern main
.extern exit
.extern environ

.section .text
_start:
    xor ebp, ebp            // Mark the outermost stack frame
    mov rdi, [rsp]          // argc
    lea rsi, [rsp + 8]      // argv
    lea rdx, [rsi + rdi*8 + 8]  // envp: the vector after argv's terminating NULL
    mov [rip + environ], rdx
    call main               // RSP is 16-byte aligned here, as the ABI requires
    mov edi, eax
    call exit
    ud2                     // exit() does not return

/*
 * A signal handler is an ordinary function, and the kernel arranges for its
 * `ret` to come here. The interrupted program's registers are in a frame on
 * the stack just above; the sigreturn system call restores them and resumes
 * the program where the signal found it. It does not return.
 */
.global __sigreturn
__sigreturn:
    mov eax, 302            // SYS_SIGRETURN
    syscall
    ud2

// No executable stack: without this note the linker assumes this object
// needs one (and binutils older than 2.45 warns, failing the CI build)
.section .note.GNU-stack,"",@progbits
