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

    // Reload Data Segment registers with Kernel Data Selector (0x10)
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

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
