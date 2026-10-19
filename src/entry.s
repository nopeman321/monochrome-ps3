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

.globl ps3_thread_entry_stub_opd
ps3_thread_entry_stub_opd:
    .long ps3_thread_entry_stub
    .long .TOC.

.globl ps3_flip_handler_cb_opd
ps3_flip_handler_cb_opd:
    .long ps3_flip_handler_cb
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

/**
 * sce_opd_call6 / sce_opd_call6f — ELFv1 import call trampoline
 *
 *   uint64_t sce_opd_call6 (uint64_t opd, uint64_t a0..a5);
 *   uint64_t sce_opd_call6f(uint64_t opd, uint64_t a0, float a1);
 *
 * r3 = address of the bound OPD [func_word, rtoc_word].  Firmware (LLE)
 * code needs r2 = its library TOC and r12 = the OPD (PowerOpen env
 * pointer) at entry, otherwise its rtoc-relative globals resolve against
 * our own .TOC. and it dereferences garbage (the 0xa0 crash).  HLE fake
 * OPDs carry rtoc=0 — harmless, the dispatcher only reads ppu.gpr.
 *
 * sce_opd_call6f shares the body: the float stays untouched in f1.
 *
 * Frame: 224 bytes = 32 linkage + GPR param save (r3-r10) + FPR param
 * save (f1-f13) + padding, so firmware callees that spill parameter
 * registers into the caller's parameter save area stay inside our frame.
 * LR and the caller's TOC ride in r14/r15 (non-volatile: the target must
 * preserve them), because some prologues write 16(r1)/24(r1) before
 * allocating their own frame — our frame slots 16/24 are then unusable.
 */
.align 2
.globl sce_opd_call6f
.globl sce_opd_call6
sce_opd_call6f:
sce_opd_call6:
    stdu  1, -0xe0(1)
    std   14, 0xd0(1)            # save caller's r14
    std   15, 0xd8(1)            # save caller's r15
    mflr  0
    mr    14, 0                  # r14 = return address (survives the target)
    mr    15, 2                  # r15 = caller's TOC
    lwz   10, 0(3)               # OPD func word
    lwz   2, 4(3)                # OPD rtoc word -> r2
    mr    12, 3                  # r12 = OPD env pointer
    mtctr 10
    mr    3, 4
    mr    4, 5
    mr    5, 6
    mr    6, 7
    mr    7, 8
    mr    8, 9
    bctrl                        # r3-r8/f1.. hold the user arguments
    mr    2, 15                  # restore caller's TOC
    mtlr  14                     # restore caller's LR
    ld    14, 0xd0(1)
    ld    15, 0xd8(1)
    addi  1, 1, 0xe0
    blr
