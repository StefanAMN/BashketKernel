.intel_syntax noprefix
.text
.global gdt_flush

// void gdt_flush(uint64_t gdtr_addr) -- rdi = pointer to struct gdt_ptr
gdt_flush:
    lgdt [rdi]

    mov ax, 0x10          // KERNEL_DATA_SEL
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    // CS can't be reloaded with a plain mov, and a direct far jump to a
    // RIP-relative label isn't expressible in this addressing mode. The
    // standard workaround: push a far pointer (selector, then offset) and
    // use a far return to pop RIP and reload CS in one step.
    push 0x08              // KERNEL_CODE_SEL
    lea rax, [rip + .Lflush]
    push rax
    retfq

.Lflush:
    ret
