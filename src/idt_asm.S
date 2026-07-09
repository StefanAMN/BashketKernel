.intel_syntax noprefix
.text
.global idt_flush

// void idt_flush(uint64_t idtr_addr) -- rdi = pointer to struct idt_ptr
idt_flush:
    lidt [rdi]
    ret
