/*
 * @file gdt_flush.S
 * @brief Assembly Helper to Load GDTR and Reload Code Segment (CS)
 * 
 * In x86_64, loading the GDTR via `lgdt` updates the table pointer, but 
 * segment selector registers (CS, DS, ES, SS) retain their old descriptors.
 * Data selectors (DS, ES, SS) can be updated with `mov ds, ax`, but CS cannot 
 * be loaded directly with `mov`. In long mode, CS must be reloaded via a 64-bit 
 * far return (`retfq`).
 */

.intel_syntax noprefix
.global gdt_flush
.global tss_flush

.section .text

gdt_flush:
    // RDI contains 1st argument: gdt_ptr address
    lgdt [rdi]

    // SS takes the Kernel Data Selector (0x10). DS, ES, FS and GS take the
    // null selector: long mode ignores them in ring 0, and they stay as they
    // are on the way to ring 3 unless the way is iretq (which clears kernel
    // selectors; SDM Vol. 2, IRET). A program returning by sysretq on a CPU
    // that had not yet done an iretq would read the kernel's selector.
    mov ax, 0x10
    mov ss, ax
    xor eax, eax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    // Reload CS with Kernel Code Selector (0x08) using far return (retfq)
    // Push Code Selector (0x08)
    push 0x08
    // Push target return offset
    lea rax, [rip + .reload_cs]
    push rax
    retfq

.reload_cs:
    ret

tss_flush:
    // Load the task register with the TSS selector in DI
    mov ax, di
    ltr ax
    ret

// No executable stack: without this note the linker assumes this object
// needs one (and binutils older than 2.45 warns, failing the CI build)
.section .note.GNU-stack,"",@progbits
