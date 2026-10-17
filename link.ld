
SECTIONS
{
    . = 0x01000000;
    .rodata : ALIGN(0x10000) { *(.rodata*) *(.eh_frame*) }
    . = ALIGN(0x10000);
    .text : ALIGN(0x10000) { *(.text*) }
    . = ALIGN(0x10000);
    .data : ALIGN(0x10000) { *(.data*) *(.got*) *(.toc*) }
    . = ALIGN(0x10000);
    .bss : ALIGN(0x10000) { *(.bss*) *(COMMON) }
}
