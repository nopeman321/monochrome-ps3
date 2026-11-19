"""
PS3 PowerPC 64-bit ELF / EBOOT.BIN Generator
Creates an authentic, compliant ELF64 PowerPC (Cell PPU) executable
with valid Program Headers, OPD (Official Procedure Descriptor),
and LV2 system call execution for RPCS3 and PS3 hardware.
"""

import struct
import os

def build_ppc64_elf():
    # Sections layout
    text_off = 0x80
    text_vaddr = 0x10080
    
    opd_off = 0xA0
    opd_vaddr = 0x100A0
    
    rodata_off = 0x100
    rodata_vaddr = 0x10100
    
    shstr_off = 0x140
    sh_tab_off = 0x180
    
    # 1. Instructions (.text)
    # PowerPC 64 Cell LV2:
    # 0x00: li r3, 0                (channel 0: stdout)
    # 0x04: lis r4, 1               (upper 16-bits of rodata vaddr: 0x0001)
    # 0x08: ori r4, r4, 0x0100      (lower 16-bits: 0x0100 -> 0x00010100)
    # 0x0C: li r5, 48               (string length = 48 bytes)
    # 0x10: li r6, 0                (pwritelen = NULL)
    # 0x14: li r11, 403             (syscall 403: sys_tty_write)
    # 0x18: sc 2                    (execute LV2 kernel syscall)
    # 0x1C: b .                     (branch-to-self: infinite loop keep thread active)
    text_code = struct.pack('>8I',
        0x38600000, # li r3, 0
        0x3C800001, # lis r4, 1
        0x60840100, # ori r4, r4, 0x100
        0x38A00030, # li r5, 48
        0x38C00000, # li r6, 0
        0x39600193, # li r11, 403
        0x44000002, # sc 2
        0x48000000  # b .
    )
    
    # 2. OPD (.opd) - Official Procedure Descriptor for PPUThread entry
    # [func_entry_vaddr, toc_base_r2, env_r11]
    opd_data = struct.pack('>QQQ', text_vaddr, opd_vaddr, 0)
    
    # 3. Rodata (.rodata)
    msg = b'[MONOCHROME PS3] PPE Thread 0 Running Cleanly!\n\x00'
    assert len(msg) == 48
    
    # 4. Section string table (.shstrtab)
    shstrtab = b'\x00.text\x00.opd\x00.rodata\x00.shstrtab\x00'
    # Offsets in shstrtab:
    # 0: ""
    # 1: ".text"
    # 7: ".opd"
    # 12: ".rodata"
    # 20: ".shstrtab"
    
    # Total image size
    total_size = sh_tab_off + (5 * 64)
    img = bytearray(total_size)
    
    # Place contents
    img[text_off:text_off+len(text_code)] = text_code
    img[opd_off:opd_off+len(opd_data)] = opd_data
    img[rodata_off:rodata_off+len(msg)] = msg
    img[shstr_off:shstr_off+len(shstrtab)] = shstrtab
    
    # 5. ELF Header (64 bytes at offset 0)
    # e_ident: [0x7f, 'E', 'L', 'F', ELFCLASS64=2, ELFDATA2MSB=2, EV_CURRENT=1, ELFOSABI_CELLLV2=0x66, ...]
    ident = b'\x7fELF\x02\x02\x01\x66' + (b'\x00' * 8)
    struct.pack_into('>16sHHIQQQIHHHHHH', img, 0,
        ident,
        2,          # e_type: ET_EXEC
        0x15,       # e_machine: EM_PPC64 (21)
        1,          # e_version: 1
        opd_vaddr,  # e_entry: points to .opd descriptor
        0x40,       # e_phoff: 64 (right after ELF header)
        sh_tab_off, # e_shoff: section header table offset
        0,          # e_flags: 0
        64,         # e_ehsize: 64 bytes
        56,         # e_phentsize: 56 bytes
        1,          # e_phnum: 1 PT_LOAD segment
        64,         # e_shentsize: 64 bytes
        5,          # e_shnum: 5 sections (NULL, .text, .opd, .rodata, .shstrtab)
        4           # e_shstrndx: section 4 is .shstrtab
    )
    
    # 6. Program Header (56 bytes at offset 0x40)
    # Elf64_Phdr: p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align
    struct.pack_into('>IIQQQQQQ', img, 0x40,
        1,          # PT_LOAD
        7,          # PF_R | PF_W | PF_X
        0,          # p_offset
        0x10000,    # p_vaddr
        0x10000,    # p_paddr
        total_size, # p_filesz
        0x10000,    # p_memsz (64 KB allocation)
        0x10000     # p_align
    )
    
    # 7. Section Header Table (5 entries, 64 bytes each, at sh_tab_off)
    # Elf64_Shdr: sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size, sh_link, sh_info, sh_addralign, sh_entsize
    # Entry 0: SHT_NULL
    # Entry 1: .text
    struct.pack_into('>IIQQQQIIQQ', img, sh_tab_off + 64*1,
        1, 1, 6, text_vaddr, text_off, len(text_code), 0, 0, 4, 0)
    # Entry 2: .opd
    struct.pack_into('>IIQQQQIIQQ', img, sh_tab_off + 64*2,
        7, 1, 3, opd_vaddr, opd_off, len(opd_data), 0, 0, 8, 24)
    # Entry 3: .rodata
    struct.pack_into('>IIQQQQIIQQ', img, sh_tab_off + 64*3,
        12, 1, 2, rodata_vaddr, rodata_off, len(msg), 0, 0, 4, 0)
    # Entry 4: .shstrtab
    struct.pack_into('>IIQQQQIIQQ', img, sh_tab_off + 64*4,
        20, 3, 0, 0, shstr_off, len(shstrtab), 0, 0, 1, 0)
        
    return bytes(img)

def generate_fself_stub(output_path):
    elf_data = build_ppc64_elf()
    os.makedirs(os.path.dirname(os.path.abspath(output_path)), exist_ok=True)
    with open(output_path, 'wb') as f:
        f.write(elf_data)
    print(f"[EBOOT] Generated valid PowerPC 64-bit ELF ({len(elf_data)} bytes) -> {output_path}")

if __name__ == '__main__':
    generate_fself_stub('pkg_build/USRDIR/EBOOT.BIN')
