/* User programs are static executables linked at 4 MiB. Each segment starts
 * on its own page so the kernel can give it its own permissions:
 * text read+execute, rodata read-only, data read+write. */
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

PHDRS
{
    text   PT_LOAD FLAGS(5); /* R + X */
    rodata PT_LOAD FLAGS(4); /* R */
    data   PT_LOAD FLAGS(6); /* R + W */
}

SECTIONS
{
    . = 0x400000;

    .text : { *(.text .text.*) } :text

    . = ALIGN(0x1000);
    .rodata : { *(.rodata .rodata.*) } :rodata

    . = ALIGN(0x1000);
    .data : { *(.data .data.*) } :data
    .bss : { *(COMMON) *(.bss .bss.*) } :data

    /DISCARD/ : { *(.eh_frame) *(.note .note.*) *(.comment) }
}
