/* Fresh OS kernel link map: higher half at -2 GiB, image byte 0 = KERNEL_VMA */
ENTRY(_start)
KERNEL_VMA = 0xFFFFFFFF80000000;

PHDRS
{
    text   PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data   PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = KERNEL_VMA;
    __kernel_start = .;

    .text : AT(ADDR(.text) - KERNEL_VMA)
    {
        *(.text.entry)
        *(.text .text.*)
    } :text

    . = ALIGN(4096);
    .rodata : AT(ADDR(.rodata) - KERNEL_VMA)
    {
        *(.rodata .rodata.*)
    } :rodata

    . = ALIGN(4096);
    .data : AT(ADDR(.data) - KERNEL_VMA)
    {
        *(.data .data.*)
    } :data

    .bss (NOLOAD) : AT(ADDR(.bss) - KERNEL_VMA)
    {
        . = ALIGN(16);
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    } :data

    . = ALIGN(4096);
    __kernel_end = .;

    /DISCARD/ :
    {
        *(.comment)
        *(.eh_frame*)
        *(.note*)
    }
}
