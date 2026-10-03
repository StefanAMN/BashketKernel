/* User programs are static position-independent executables: linked as if
 * loaded at address 0, and placed at a random address by the kernel, which
 * then applies the relocations listed in .rela.dyn (see src/fs/elf_dyn.h).
 *
 * Each segment starts on its own page so that it can have its own
 * permissions: text read+execute, rodata read-only, data read+write. */
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

PHDRS
{
    text    PT_LOAD    FLAGS(5); /* R + X */
    rodata  PT_LOAD    FLAGS(4); /* R */
    data    PT_LOAD    FLAGS(6); /* R + W */
    dynamic PT_DYNAMIC FLAGS(6); /* Points into the data segment: where the relocations are */
}

SECTIONS
{
    . = 0;

    .text : { *(.text .text.*) } :text

    . = ALIGN(0x1000);
    .rodata : { *(.rodata .rodata.*) } :rodata
    .rela.dyn : { *(.rela .rela.*) } :rodata   /* The list of addresses to fix up */
    .dynsym : { *(.dynsym) } :rodata
    .dynstr : { *(.dynstr) } :rodata
    .hash : { *(.hash) } :rodata
    .gnu.hash : { *(.gnu.hash) } :rodata

    . = ALIGN(0x1000);
    .dynamic : { *(.dynamic) } :data :dynamic
    .got : { *(.got .got.plt) } :data
    .data : { *(.data.rel.ro .data.rel.ro.*) *(.data .data.*) } :data
    .bss : { *(COMMON) *(.bss .bss.*) } :data

    /DISCARD/ : { *(.eh_frame) *(.note .note.*) *(.comment) *(.interp) }
}
