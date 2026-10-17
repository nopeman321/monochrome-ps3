/**
 * GameOS PPU Official Procedure Descriptor (OPD) and Thread Boot Stub
 *
 * PS3 GameOS LV2 sysPrxForUser dereferences the entry point as a 32-bit OPD:
 *   [uint32_t entry_function_address, uint32_t toc_base_address]
 */
.section .opd, "aw"
.align 3
.globl ps3_entry_opd
ps3_entry_opd:
    .long _start_ps3
    .long .TOC.

.section .text, "ax"
.globl _start_ps3
_start_ps3:
    # Explicitly guarantee r2 is loaded with .TOC. from OPD descriptor
    lis 2, ps3_entry_opd@ha
    addi 2, 2, ps3_entry_opd@l
    lwz 2, 4(2)

    # Allocate initial 128-byte PowerPC ABI stack frame
    stdu 1, -128(1)

    # Call C entry point (main via ps3_main_entry)
    bl ps3_main_entry
    nop

    # If ps3_main_entry returns, terminate process cleanly (Syscall 3)
    li 11, 3
    sc

1:  b 1b
