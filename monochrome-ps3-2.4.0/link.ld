SECTIONS
{
    . = 0x01000000;
    .opd : ALIGN(0x10000) { *(.opd*) }
    . = ALIGN(0x10000);
    .text : ALIGN(0x10000) { *(.text*) *(.init*) *(.fini*) }
    . = ALIGN(0x10000);
    .rodata : ALIGN(0x10000) { *(.rodata*) *(.eh_frame*) KEEP(*(.sys_proc_param*)) }
    . = ALIGN(0x10000);
    .data : ALIGN(0x10000) { *(.data*) *(.got*) *(.toc*) *(.branch_lt*) }
    . = ALIGN(0x10000);
    .bss : ALIGN(0x10000) { *(.bss*) *(COMMON) }
}
