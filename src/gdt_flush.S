.intel_syntax noprefix

.global gdt_flush
gdt_flush:
    // rdi contains the pointer to our gdtr struct
    lgdt [rdi]

    // Load data segment registers (index 2 in GDT = 0x10)
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    // Far return to reload CS
    // Push the CS selector (0x08)
    push 0x08
    // Push the address we want to return to
    lea rax, [rip + .flush_cs]
    push rax
    // retfq pops RIP, then pops CS.
    retfq

.flush_cs:
    // Load the TSS
    // Index 5 in GDT = 5 * 8 = 40 (0x28).
    // The selector is 0x28.
    mov ax, 0x28
    ltr ax

    ret
