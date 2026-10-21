#!/usr/bin/env python3
"""sce_imports.py — SCE dynamic import table patcher for Monochrome PS3.

Appends to the linked PPU ELF64 (build/EBOOT.ELF) two extra program headers:

  * PT_LOAD (read-only) covering a small appended segment that contains
      - a ppu_proc_prx_param_t structure (magic 0x1B434CEC)
      - one 1-function ppu_prx_module_info import descriptor per imported
        function (cellGcmSys / sys_io / cellAudio); each descriptor's
        `addrs` field points at that function's .data slot, and its `nids`
        field points at its own NID, so the loader never writes into the
        blob itself
      - module name strings and the NID table
  * PT 0x60000002 (LOOS+2) pointing at the param structure.  This is the
    mechanism RPCS3 (and the real GameOS loader) uses to discover static
    imports of a standalone EXEC.  On boot, the loader resolves every NID
    against the registered HLE modules and writes the 32-bit address of the
    function descriptor (OPD) into the low half of each 64-bit slot defined
    in src/platform_ps3.c (symbols named sce_slot_<function>).  The C code
    calls through the slot value using the standard ELFv1 indirect-call
    convention, so a statically linked ELF now binds HLE functions at load
    time without any PRX.

NIDs are SHA-1 derived (identical for real firmware sprx exports):

    NID = little-endian u32 of SHA1(name || 0x6759659904250490566427499489741A)[0:4]

Usage:  python tools/sce_imports.py build/EBOOT.ELF
"""
import hashlib
import struct
import sys

NID_SUFFIX = bytes.fromhex("6759659904250490566427499489741A")

MAGIC_PROC_PRX_PARAM = 0x1B434CEC
PT_LOAD = 1
PT_SCE_PROC_PRX_PARAM = 0x60000002

# Lives inside the 16..272 MB window RPCS3 reserves for the main image
# (alloc_at = max(vaddr & -0x10000000, 0x10000)); far above the .bss end
# (0x029A85C0) and below the 0x30000000 loader limit.
SEG_VADDR = 0x03000000

# module -> firmware export names to import.  Every name FOO requires a
# 64-bit slot variable named sce_slot_FOO in src/platform_ps3.c.
IMPORTS = [
    ("cellGcmSys", [
        "_cellGcmInitBody",
        "cellGcmSetFlipMode",
        "cellGcmSetFlip",
        "cellGcmSetFlipImmediate",
        "cellGcmSetDisplayBuffer",
        "cellGcmMapMainMemory",
        "cellGcmGetFlipStatus",
        "cellGcmResetFlipStatus",
        "cellGcmSetFlipHandler",
        "cellGcmGetLastFlipTime",
        "cellGcmSetWaitFlip",
        "cellGcmGetConfiguration",
    ]),
    # cellPad* functions live in retail libsys_io.sprx; RPCS3 registers them
    # under the module name "sys_io" (REG_FUNC(sys_io, cellPadInit)).  Using
    # "cellPad" here made every pad NID unresolvable -> null-bound -> every
    # poll logged "PPU: Unregistered function called".
    ("sys_io", [
        "cellPadInit",
        "cellPadGetData",
    ]),
    ("cellAudio", [
        "cellAudioInit",
        "cellAudioPortOpen",
        "cellAudioGetPortConfig",
        "cellAudioPortStart",
        "cellAudioPortStop",
        "cellAudioPortClose",
        "cellAudioSetPortLevel",
    ]),
]


def nid(name):
    h = hashlib.sha1(name.encode("ascii") + NID_SUFFIX).digest()
    return int.from_bytes(h[:4], "little")


def align4(n):
    return (n + 3) & ~3


def main(elf_path):
    d = bytearray(open(elf_path, "rb").read())

    if bytes(d[:6]) != b"\x7fELF\x02\x02":
        sys.exit("sce_imports: %s is not a big-endian ELF64 file" % elf_path)

    u16 = lambda o: struct.unpack_from(">H", d, o)[0]
    u32 = lambda o: struct.unpack_from(">I", d, o)[0]
    u64 = lambda o: struct.unpack_from(">Q", d, o)[0]

    e_phoff, e_shoff = u64(32), u64(40)
    e_phentsize, e_phnum = u16(54), u16(56)
    e_shentsize, e_shnum, e_shstrndx = u16(58), u16(60), u16(62)
    if e_phentsize != 56:
        sys.exit("sce_imports: unexpected e_phentsize %d" % e_phentsize)

    for i in range(e_phnum):
        p_type = u32(e_phoff + i * e_phentsize)
        if p_type == PT_SCE_PROC_PRX_PARAM:
            sys.exit("sce_imports: ELF already patched")

    # ── symbol table ────────────────────────────────────────────────────
    sho = e_shoff + e_shstrndx * e_shentsize
    shstr_off, shstr_size = u64(sho + 24), u64(sho + 32)
    shstr = bytes(d[shstr_off:shstr_off + shstr_size])
    sections = {}
    for i in range(e_shnum):
        o = e_shoff + i * e_shentsize
        nm = u32(o)
        name = shstr[nm:shstr.find(b"\0", nm)].decode("latin1")
        sections[name] = (u64(o + 24), u64(o + 32))
    if ".symtab" not in sections or ".strtab" not in sections:
        sys.exit("sce_imports: ELF has no .symtab/.strtab")

    soff, ssz = sections[".symtab"]
    stroff, strsz = sections[".strtab"]
    strtab = bytes(d[stroff:stroff + strsz])
    symbols = {}
    for k in range(0, ssz, 24):  # ELF64 Sym = 24 bytes
        st_name, st_info, st_other, st_shndx, st_value, st_size = \
            struct.unpack_from(">IBBHQQ", d, soff + k)
        if st_name and st_value:
            nm = strtab[st_name:strtab.find(b"\0", st_name)].decode("latin1")
            symbols.setdefault(nm, st_value)

    # ── collect imports ─────────────────────────────────────────────────
    # RPCS3's ppu_load_imports() writes the resolved OPD address to a u32
    # slot:  faddr = descriptor.addrs[i]; *(u32*)faddr = link_addr.  Our
    # sce_slot_* variables are 64-bit and sit 8 bytes apart, so a single
    # per-module addrs array (striding 4) can never line up with them — and
    # if the array lived in this blob the loader would fault writing into
    # the read-only segment (the original "Access violation writing
    # location 0x300010c" crash).  We therefore emit ONE 1-function
    # descriptor per import whose `addrs` field points directly at
    # slot+4 — the low half of the big-endian u64.  The loader stores the
    # OPD pointer there (in .data, writable) and the existing C code reads
    # the zero-extended u64 unchanged.  Nothing in the blob is ever written.
    flat = []  # (modname, func, nid, slot_addr)
    for modname, funcs in IMPORTS:
        for fn in funcs:
            slot = symbols.get("sce_slot_" + fn)
            if not slot:
                sys.exit("sce_imports: missing slot symbol sce_slot_%s "
                         "(define it in src/platform_ps3.c)" % fn)
            flat.append((modname, fn, nid(fn), slot))
            print("sce_imports: %-11s %-30s nid=%08x slot+4=0x%08x"
                  % (modname, fn, nid(fn), slot + 4))

    total_funcs = len(flat)

    # ── build the appended segment blob ─────────────────────────────────
    # layout: [param 0x28][descriptors 0x2C*F][unique names][nids 4*F]
    desc_total = 0x2C * total_funcs

    module_names, seen = [], set()
    for modname, _, _, _ in flat:
        if modname not in seen:
            seen.add(modname)
            module_names.append(modname)
    names_size = sum(len(m) + 1 for m in module_names)

    off_param = 0x00
    off_descs = 0x28
    off_names = off_descs + desc_total
    off_nids = align4(off_names + names_size)
    blob_size = off_nids + 4 * total_funcs

    blob = bytearray(blob_size)

    # module name strings (one copy per module, shared by its descriptors)
    name_ptr, cur = {}, off_names
    for m in module_names:
        name_ptr[m] = SEG_VADDR + cur
        nb = m.encode("ascii") + b"\0"
        blob[cur:cur + len(nb)] = nb
        cur += len(nb)

    # descriptors + nid table
    desc_off, nid_off = off_descs, off_nids
    for modname, fn, n, slot in flat:
        struct.pack_into(">BBHHHHHBB2s7I", blob, desc_off,
                         0x2C, 0x00,          # size, unk0
                         0x0101,              # version
                         0x0000,              # attributes (no frefs, no vars)
                         1, 0, 0,             # num_func=1, num_var, num_tlsvar
                         0x00, 0x00,          # info_hash, info_tlshash
                         b"\0\0",             # unk1
                         name_ptr[modname],   # name
                         SEG_VADDR + nid_off, # nids -> this function's nid
                         slot + 4,            # addrs -> .data slot+4 (loader writes OPD)
                         0, 0,                # vnids, vstubs
                         0, 0)                # unk4, unk5
        struct.pack_into(">I", blob, nid_off, n)
        desc_off += 0x2C
        nid_off += 4

    # param structure — libstub range covers the contiguous descriptors
    struct.pack_into(">IIIIIIIIHHI", blob, off_param,
                     0x28,                 # size of this structure
                     MAGIC_PROC_PRX_PARAM,
                     1,                    # version
                     0,                    # unk0
                     0, 0,                 # libent_start / libent_end (none)
                     SEG_VADDR + off_descs,
                     SEG_VADDR + off_descs + desc_total,
                     0x0101, 0,            # ver / unk1
                     0)                    # unk2

    # ── append segment blob + rebuild program header table ──────────────
    # scetool records each metadata section's program_idx as `i - skip`,
    # where `skip` counts the non-LOAD program headers seen so far (see
    # _build_self_64 / sce_set_metash in tools/scetool_fork).  That formula
    # is only correct when every PT_LOAD occupies a contiguous index starting
    # at 0.  lld emits GNU_EH_FRAME/GNU_STACK in the middle of the table, so
    # a PT_LOAD appended after them is stored with a stale program_idx and
    # RPCS3 ends up pairing it with the wrong segment (it decompressed our
    # 332-byte import blob into GNU_EH_FRAME's 19956-byte buffer).  To keep
    # the LOAD indices contiguous we INSERT the blob's PT_LOAD right after
    # the real LOAD segments and push the non-LOAD headers to the tail.
    file_off = (len(d) + 0xFFF) & ~0xFFF
    d.extend(b"\0" * (file_off - len(d)))
    d.extend(blob)

    orig = [bytes(d[e_phoff + i * e_phentsize:
                    e_phoff + (i + 1) * e_phentsize]) for i in range(e_phnum)]

    # PT_LOAD covering the whole blob (read-only).
    blob_phdr = struct.pack(">IIQQQQQQ", PT_LOAD, 4,
                            file_off, SEG_VADDR, SEG_VADDR,
                            blob_size, blob_size, 0x10000)
    # PT 0x60000002 pointing at the param struct (skipped by scetool: it is
    # not a LOAD type, so it never enters the metadata section table).
    param_phdr = struct.pack(">IIQQQQQQ", PT_SCE_PROC_PRX_PARAM, 4,
                             file_off + off_param, SEG_VADDR + off_param,
                             SEG_VADDR + off_param,
                             0x28, 0x28, 0x4)

    loads, others = [], []
    for ph in orig:
        p_type = struct.unpack_from(">I", ph, 0)[0]
        (loads if p_type == PT_LOAD else others).append(ph)

    # ── sanitizado para GameOS/LV2 real ─────────────────────────────────────
    # El loader de una PS3 fisica solo entiende PT_LOAD y el header
    # SCE_PROC_PRX_PARAM (0x60000002). Cualquier otro tipo (GNU_EH_FRAME
    # 0x6474e550, GNU_STACK 0x6474e551, PT_TLS 7) hace que el arranque falle
    # con 80010007 ("error en la operacion de inicio"). RPCS3 los ignoraba.
    PT_TLS = 7
    PT_GNU_EH_FRAME = 0x6474E550
    PT_GNU_STACK = 0x6474E551
    DROP_TYPES = {PT_TLS, PT_GNU_EH_FRAME, PT_GNU_STACK}
    others = [ph for ph in others
              if struct.unpack_from(">I", ph, 0)[0] not in DROP_TYPES]

    # EI_OSABI debe ser 0x66 (ELFOSABI_CELL_LV2) en un ejecutable de PS3.
    d[7] = 0x66

    # All LOADs (originals + blob) stay contiguous at the front.
    table = loads + [blob_phdr] + others + [param_phdr]
    new_phnum = len(table)

    end_old = e_phoff + e_phnum * e_phentsize
    end_new = e_phoff + new_phnum * e_phentsize
    if any(d[end_old:end_new]):
        sys.exit("sce_imports: no room for extra program headers")

    for k, ph in enumerate(table):
        d[e_phoff + k * e_phentsize: e_phoff + (k + 1) * e_phentsize] = ph

    struct.pack_into(">H", d, 56, new_phnum)

    open(elf_path, "wb").write(bytes(d))
    print("sce_imports: patched %s (+%d bytes at 0x%x, %d imports in %d modules, "
          "%d phdrs, blob at index %d)"
          % (elf_path, blob_size, file_off, total_funcs, len(module_names),
             new_phnum, len(loads)))


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: sce_imports.py <EBOOT.ELF>")
    main(sys.argv[1])
