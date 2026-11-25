#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
pkg3.py -- Implementacion de referencia en Python 3 del formato PKG3 de PS3.

Genera paquetes NO FINALIZADOS (debug/free), que son los que instalan en
hardware real (PS3HEN / CFW) sin necesidad de las claves NPDRM de Sony.

Sin dependencias externas. Solo hashlib + struct.

Comandos:
    python pkg3.py build   <carpeta> <salida.pkg> [opciones]
    python pkg3.py verify  <archivo.pkg>
    python pkg3.py extract <archivo.pkg> <destino>

Formato implementado (todo big-endian):

  0x000  0x080  Cabecera principal
  0x080  0x010  SHA1(cabecera)[3:19]
  0x090  0x030  crypt(ctx=sha1_meta, 0x30 ceros) -> vuelto a cifrar con ctx=sha1_hdr
  0x0C0  0x040  Bloque de metadatos TLV (5 entradas)
  0x100  0x010  SHA1(metadatos)[3:19]
  0x110  0x030  crypt(ctx=sha1_meta, 0x30 ceros)
  0x140  N      Region de datos (tabla de items + nombres + ficheros), cifrada
  0x140+N 0x60  Relleno final a cero
"""

import argparse
import hashlib
import os
import struct
import sys

# ----------------------------------------------------------------------------
# Constantes del formato
# ----------------------------------------------------------------------------

PKG_MAGIC = 0x7F504B47          # "\x7fPKG"
PKG_TYPE_PS3 = 0x0001           # 0x0002 = PSP / PS Vita
PKG_REV_DEBUG = 0x0000          # no finalizado
PKG_REV_FINAL = 0x8000          # finalizado (retail, firmado)

HEADER_SIZE = 0x80
METADATA_OFFSET = 0xC0
METADATA_SIZE_FIELD = 0x80      # tamanio de la region de metadatos
METADATA_TLV_SIZE = 0x40        # 5 entradas TLV de este generador
DATA_OFFSET = 0x140
TAIL_SIZE = 0x60
ITEM_ENTRY_SIZE = 0x20

# DRMType (id de metadato 0x1)
DRM_NETWORK = 1
DRM_LOCAL = 2
DRM_FREE = 3

# ContentType (id de metadato 0x2)
CT_GAMEDATA = 0x04
CT_GAMEEXEC = 0x05

# PackageType / PackageFlag (id de metadato 0x3)
PT_DEMO_KEY = 0x08
PT_NORMAL = 0x0E
PT_CUMULATIVE_PATCH = 0x10
PF_CUMULATIVE = 0x10
PF_RENAME_DIR = 0x40
PF_DISC_BINDED = 0x800

# Flags por entrada de la tabla de items
F_NPDRMSELF = 0x1
F_EDAT = 0x2
F_RAW = 0x3
F_DIRECTORY = 0x4
F_OVERWRITE = 0x80000000

# Revision de make_package_npdrm que declaramos (id de metadato 0x5)
MAKE_PKG_REVISION = 0x1061

EBOOT_META_MAGIC = 0x4E504400   # "NPD\0"

SELF_MAGIC = b"SCE\x00\x00\x00\x00\x02\x80"
SELF_HEADER_SIZE = 0x30
CONTROL_INFO_ENTRY_SIZE = 0x30


# ----------------------------------------------------------------------------
# Cifrado "non finalized" (XOR con keystream SHA-1)
# ----------------------------------------------------------------------------

def key_to_context(key):
    """Construye el contexto de 0x40 bytes a partir de los 16 bytes de digest.

    [0x00:0x08] = key[0:8]
    [0x08:0x10] = key[0:8]
    [0x10:0x18] = key[8:16]
    [0x18:0x20] = key[8:16]
    [0x20:0x38] = ceros
    [0x38:0x40] = contador u64 big-endian (aqui 0)
    """
    ctx = bytearray(0x40)
    ctx[0x00:0x08] = key[0:8]
    ctx[0x08:0x10] = key[0:8]
    ctx[0x10:0x18] = key[8:16]
    ctx[0x18:0x20] = key[8:16]
    # [0x20:0x38] ya son ceros
    return ctx


def set_context_num(ctx, num):
    """Escribe el contador u64 big-endian en ctx[0x38:0x40]."""
    ctx[0x38:0x40] = struct.pack(">Q", num & 0xFFFFFFFFFFFFFFFF)


def keystream_block(key, index):
    """16 bytes de keystream para el bloque numero `index`.

    Es simplemente SHA1(contexto) truncado a 16 bytes, donde el contexto
    lleva el indice de bloque en sus ultimos 8 bytes.
    """
    ctx = key_to_context(key)
    set_context_num(ctx, index)
    return hashlib.sha1(bytes(ctx)).digest()[:16]


def pkg_crypt(key, data, start_block=0):
    """XOR del keystream sobre `data`. Cifra y descifra son la misma operacion."""
    out = bytearray(data)
    for off in range(0, len(out), 16):
        ks = keystream_block(key, start_block + off // 16)
        n = min(16, len(out) - off)
        for i in range(n):
            out[off + i] ^= ks[i]
    return bytes(out)


# ----------------------------------------------------------------------------
# Utilidades
# ----------------------------------------------------------------------------

def pad16(data):
    """Rellena con ceros hasta multiplo de 16."""
    rem = len(data) % 16
    return data if rem == 0 else data + b"\x00" * (16 - rem)


def sha1(data):
    return hashlib.sha1(data).digest()


def eboot_declared_size(real_size):
    """Tamanio que se declara para USRDIR/EBOOT.BIN (ajuste de 0x30 + 64)."""
    return ((real_size - 0x30 + 63) & ~63) + 0x30


def build_npdrm_eboot(original, file_sha1, content_id):
    """Sustituye el bloque de 0x80 bytes posterior al control-info de un SELF
    NPDRM por la cabecera NPD (EbootMeta). Devuelve (nuevo_contenido, ok).
    """
    if len(original) < SELF_HEADER_SIZE or original[0:9] != SELF_MAGIC:
        return original, False

    app_info_off = struct.unpack_from(">Q", original, 0x28)[0]
    control_info_off = struct.unpack_from(">Q", original, 0x58)[0]

    app_type = 0
    if app_info_off + 0x10 <= len(original):
        app_type = struct.unpack_from(">I", original, app_info_off + 0x0C)[0]

    # Recorrer los bloques de control-info hasta encontrar el de tipo 3
    off = control_info_off
    found = False
    while off + CONTROL_INFO_ENTRY_SIZE <= len(original):
        ctype, csize = struct.unpack_from(">II", original, off)
        is_next = struct.unpack_from(">Q", original, off + 8)[0]
        if ctype == 3:
            found = True
            break
        off += csize
        if is_next != 1:
            break
    off += CONTROL_INFO_ENTRY_SIZE

    if app_type != 8 or not found or off + 0x80 > len(original):
        return original, False

    meta = bytearray(0x80)
    struct.pack_into(">I", meta, 0x00, EBOOT_META_MAGIC)
    struct.pack_into(">I", meta, 0x04, 1)          # unk1
    struct.pack_into(">I", meta, 0x08, DRM_FREE)   # drmType
    struct.pack_into(">I", meta, 0x0C, 1)          # unk2
    cid = content_id.encode("ascii")[:0x30]
    meta[0x10:0x10 + len(cid)] = cid

    for i in range(0x10):
        b = file_sha1[i]
        meta[0x40 + i] = b
        nb = (~b) & 0xFF
        meta[0x50 + i] = nb
        meta[0x60 + i] = ((1 if i == 0x0F else 0) ^ nb ^ 0xAA) & 0xFF
        meta[0x70 + i] = 0

    return original[:off] + bytes(meta) + original[off + 0x80:], True


# ----------------------------------------------------------------------------
# Construccion de la cabecera y del bloque de metadatos
# ----------------------------------------------------------------------------

def build_metadata_tlv(data_size, content_type, package_flags, drm_type):
    """5 entradas TLV (id u32, size u32, valor) = 0x40 bytes exactos.

    0x01 / 4   DRMType
    0x02 / 4   ContentType
    0x03 / 4   PackageType + PackageFlag + StorageType
    0x04 / 8   Package Size (tamanio de la region de datos, u64)
    0x05 / 4   make_package_npdrm Revision (u16) + Package Version (u16)
    """
    tlv = bytearray()
    tlv += struct.pack(">III", 0x01, 4, drm_type)
    tlv += struct.pack(">III", 0x02, 4, content_type)
    tlv += struct.pack(">III", 0x03, 4, package_flags)
    tlv += struct.pack(">IIQ", 0x04, 8, data_size)
    tlv += struct.pack(">III", 0x05, 4,
                       (MAKE_PKG_REVISION << 16) | 0x0000)
    assert len(tlv) == METADATA_TLV_SIZE, len(tlv)
    return bytes(tlv)


def build_header(item_count, total_size, data_size, content_id):
    """Cabecera principal de 0x08..0x80 (con contentID/digest/riv a cero)."""
    hdr = bytearray(HEADER_SIZE)
    struct.pack_into(">I", hdr, 0x00, PKG_MAGIC)
    struct.pack_into(">H", hdr, 0x04, PKG_REV_DEBUG)   # no finalizado
    struct.pack_into(">H", hdr, 0x06, PKG_TYPE_PS3)
    struct.pack_into(">I", hdr, 0x08, METADATA_OFFSET)
    struct.pack_into(">I", hdr, 0x0C, 5)               # metadata_count
    struct.pack_into(">I", hdr, 0x10, METADATA_SIZE_FIELD)
    struct.pack_into(">I", hdr, 0x14, item_count)
    struct.pack_into(">Q", hdr, 0x18, total_size)
    struct.pack_into(">Q", hdr, 0x20, DATA_OFFSET)
    struct.pack_into(">Q", hdr, 0x28, data_size)
    # 0x30 contentID, 0x60 digest, 0x70 pkg_data_riv: se rellenan despues
    return hdr


# ----------------------------------------------------------------------------
# Recoleccion de ficheros
# ----------------------------------------------------------------------------

class Item(object):
    __slots__ = ("name", "raw", "is_dir", "flags", "data", "name_off",
                 "data_off", "size", "plen", "adjust")

    def __init__(self, name, is_dir, raw=b"", flags=F_RAW):
        self.name = name          # str, ruta relativa con "/"
        self.is_dir = is_dir
        self.raw = raw            # bytes tal cual estan en disco
        self.data = raw           # bytes que van al paquete (tras transformar)
        self.flags = flags
        self.name_off = 0
        self.data_off = 0
        self.size = 0             # tamanio DECLARADO en la tabla de items
        self.plen = 0             # bytes realmente ocupados (multiplo de 16)
        self.adjust = False       # aplica el ajuste de 0x30+64 de EBOOT.BIN


def collect_items(root, content_id, absolute_root=None, verbose=False):
    """Devuelve la lista de items: directorios primero (padres antes que hijos)
    y luego ficheros, ambos en orden alfabetico.

    Si `absolute_root` no es None, cada nombre se prefija con
    "../../../<absolute_root>/" para instalar fuera de /dev_hdd0/game/<TITLE_ID>.
    """
    dirs, files = [], []

    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        filenames.sort()
        rel_dir = os.path.relpath(dirpath, root)
        if rel_dir != ".":
            dirs.append(rel_dir.replace(os.sep, "/"))
        for fn in filenames:
            full = os.path.join(dirpath, fn)
            rel = os.path.relpath(full, root).replace(os.sep, "/")
            files.append((rel, full))

    items = []
    for d in dirs:
        items.append(Item(d, True, flags=F_OVERWRITE | F_DIRECTORY))

    for rel, full in files:
        with open(full, "rb") as fp:
            raw = fp.read()

        it = Item(rel, False, raw=raw, flags=F_OVERWRITE | F_RAW)

        # EBOOT.BIN: si es un SELF NPDRM se le inyecta la cabecera NPD
        if rel.upper() == "USRDIR/EBOOT.BIN":
            it.flags = F_OVERWRITE | F_NPDRMSELF
            fsha = sha1(raw)
            newdata, injected = build_npdrm_eboot(raw, fsha, content_id)
            it.data = newdata
            it.adjust = injected
            if verbose:
                sys.stderr.write(
                    "  EBOOT.BIN: %d bytes, NPDRM SELF %s\n"
                    % (len(raw), "detectado (cabecera NPD inyectada)"
                       if injected else "no detectado (se copia tal cual)"))

        items.append(it)

    if absolute_root:
        root_path = absolute_root.replace("\\", "/").strip("/")
        prefix = "../../../" + root_path
        out = []
        # Un item de directorio por cada nivel intermedio, empezando por el
        # primer componente real ("dev_hdd0"). Nunca se emite "../.." ni nada
        # compuesto solo de "..".
        parts = prefix.split("/")
        for i in range(1, len(parts) + 1):
            sub = "/".join(parts[:i])
            comps = [c for c in sub.split("/") if c and c != ".."]
            if not comps:
                continue
            out.append(Item(sub, True, flags=F_OVERWRITE | F_DIRECTORY))
        for it in items:
            it.name = prefix + "/" + it.name
            out.append(it)
        items = out

    return items


# ----------------------------------------------------------------------------
# Empaquetado
# ----------------------------------------------------------------------------

def build_pkg(root, out_path, content_id, content_type=CT_GAMEEXEC,
              package_flags=PT_NORMAL, drm_type=DRM_FREE,
              absolute_root=None, verbose=False):
    """Construye el .pkg y devuelve un resumen (dict)."""

    content_id = content_id.strip()
    if len(content_id) != 36:
        raise SystemExit(
            "ContentID debe tener 36 caracteres "
            "('<XX0000>-<TITLEID>_00-<16 hex>'), recibidos %d: %r"
            % (len(content_id), content_id))
    if not content_id.isascii():
        raise SystemExit("ContentID debe ser ASCII")

    items = collect_items(root, content_id, absolute_root, verbose)
    if not items:
        raise SystemExit("'%s' no contiene ficheros" % root)

    # --- reparto de offsets dentro de la region de datos -------------------
    off = ITEM_ENTRY_SIZE * len(items)
    for it in items:
        it.name_off = off
        off += len(pad16(it.name.encode("ascii")))
    descriptor_size = off

    for it in items:
        if it.is_dir:
            it.size = 0
            it.plen = 0
            it.data_off = off
            continue
        it.data_off = off
        if it.flags & 0xFF == F_NPDRMSELF and it.adjust:
            # El tamanio declarado lleva el ajuste de 0x30 + 64 (PSL1GHT).
            # Solo aplica cuando de verdad se inyecto la cabecera NPD; el SELF
            # NPDRM real ya viene alineado a 64 desde el SDK.
            it.size = eboot_declared_size(len(it.data))
        else:
            it.size = len(it.data)
        it.plen = len(pad16(it.data.ljust(max(it.size, len(it.data)), b"\x00")))
        it.data = it.data.ljust(it.plen, b"\x00")
        off += it.plen

    data_size = off

    # --- area de descriptores (tabla + nombres) ---------------------------
    table = bytearray()
    for it in items:
        name = it.name.encode("ascii")
        table += struct.pack(
            ">IIQQII",
            it.name_off,
            len(name),
            it.data_off,
            it.size,
            it.flags,
            0,
        )
    for it in items:
        table += pad16(it.name.encode("ascii"))
    descriptor = bytes(table)
    assert len(descriptor) == descriptor_size

    # --- cabecera (aun con contentID/digest/riv a cero) -------------------
    total_size = DATA_OFFSET + data_size + TAIL_SIZE
    hdr = build_header(len(items), total_size, data_size, content_id)

    # --- QA digest --------------------------------------------------------
    # sha1(ficheros crudos en orden || cabecera_con_campos_a_cero || descriptores)
    # y se toman los 16 PRIMEROS bytes.
    qa = hashlib.sha1()
    for it in items:
        if not it.is_dir:
            qa.update(it.raw)
    qa.update(bytes(hdr))
    qa.update(descriptor)
    digest = qa.digest()[:16]

    # --- rellenar cabecera ------------------------------------------------
    cid = content_id.encode("ascii")
    hdr[0x30:0x30 + len(cid)] = cid
    hdr[0x60:0x70] = digest
    # pkg_data_riv = 16 ceros cifrados con el contexto del digest y contador
    # 0xFFFFFFFFFFFFFFFF (es la "K Licensee" cifrada)
    hdr[0x70:0x80] = pkg_crypt(digest, b"\x00" * 16, start_block=0xFFFFFFFFFFFFFFFF)

    # --- digests de cabecera y de metadatos -------------------------------
    hdr_sha16 = sha1(bytes(hdr))[3:19]
    meta = build_metadata_tlv(data_size, content_type, package_flags, drm_type)
    meta_sha16 = sha1(meta)[3:19]

    pad_meta = pkg_crypt(meta_sha16, b"\x00" * 0x30)          # 0x110..0x140
    pad_hdr = pkg_crypt(hdr_sha16, pad_meta)                  # 0x090..0x0C0

    # --- region de datos en claro ----------------------------------------
    plain = bytearray(descriptor)
    for it in items:
        if not it.is_dir:
            plain += it.data
    assert len(plain) == data_size

    payload = pkg_crypt(digest, bytes(plain))

    # --- escribir ---------------------------------------------------------
    with open(out_path, "wb") as fp:
        fp.write(bytes(hdr))          # 0x000
        fp.write(hdr_sha16)           # 0x080
        fp.write(pad_hdr)             # 0x090
        fp.write(meta)                # 0x0C0
        fp.write(meta_sha16)          # 0x100
        fp.write(pad_meta)            # 0x110
        fp.write(payload)             # 0x140
        fp.write(b"\x00" * TAIL_SIZE)  # cola

    written = os.path.getsize(out_path)
    if written != total_size:
        raise SystemExit("tamanio escrito %d != total_size %d"
                         % (written, total_size))

    return {
        "path": out_path,
        "items": len(items),
        "files": sum(1 for i in items if not i.is_dir),
        "data_size": data_size,
        "total_size": total_size,
        "digest": digest,
        "content_id": content_id,
    }


# ----------------------------------------------------------------------------
# Lectura / verificacion / extraccion (para comprobar el round-trip)
# ----------------------------------------------------------------------------

def parse_pkg(path):
    with open(path, "rb") as fp:
        blob = fp.read()

    (magic, revision, pkg_type, meta_off, meta_cnt, meta_size,
     item_count, total_size, data_off, data_size) = struct.unpack_from(
        ">IHHIIIIQQQ", blob, 0)

    if magic != PKG_MAGIC:
        raise SystemExit("magic incorrecto: 0x%08X" % magic)

    out = {
        "revision": revision,
        "finalized": bool(revision & 0x8000),
        "pkg_type": pkg_type,
        "metadata_offset": meta_off,
        "metadata_count": meta_cnt,
        "metadata_size": meta_size,
        "item_count": item_count,
        "total_size": total_size,
        "data_offset": data_off,
        "data_size": data_size,
        "file_size": len(blob),
        "content_id": blob[0x30:0x60].split(b"\x00")[0].decode("ascii", "replace"),
        "digest": blob[0x60:0x70],
        "riv": blob[0x70:0x80],
        "header_sha16": blob[0x80:0x90],
        "meta_sha16": blob[0x100:0x110],
    }

    # Integridad estructural
    problems = []
    if total_size != len(blob):
        problems.append("total_size (%d) != tamanio real (%d)"
                        % (total_size, len(blob)))
    if data_off + data_size + TAIL_SIZE != total_size:
        problems.append("data_offset + data_size + 0x60 != total_size")
    if blob[total_size - TAIL_SIZE:] != b"\x00" * TAIL_SIZE:
        problems.append("la cola de 0x60 bytes no es cero")

    # Digest de cabecera
    if sha1(blob[0:0x80])[3:19] != blob[0x80:0x90]:
        problems.append("SHA1(cabecera)[3:19] no coincide")
    # Bloque de metadatos
    meta_block = blob[meta_off:meta_off + METADATA_TLV_SIZE]
    if sha1(meta_block)[3:19] != blob[0x100:0x110]:
        problems.append("SHA1(metadatos)[3:19] no coincide")
    # Rellenos cifrados
    if pkg_crypt(blob[0x100:0x110], b"\x00" * 0x30) != blob[0x110:0x140]:
        problems.append("relleno cifrado 0x110 no coincide")
    if pkg_crypt(blob[0x80:0x90], blob[0x110:0x140]) != blob[0x90:0xC0]:
        problems.append("relleno cifrado 0x090 no coincide")
    # RIV
    if pkg_crypt(blob[0x60:0x70], b"\x00" * 16,
                 start_block=0xFFFFFFFFFFFFFFFF) != blob[0x70:0x80]:
        problems.append("pkg_data_riv no coincide con el keystream esperado")

    out["problems"] = problems

    # Descriptores TLV
    tlv = []
    p = meta_off
    for _ in range(meta_cnt):
        if p + 8 > len(blob):
            break
        tid, tsize = struct.unpack_from(">II", blob, p)
        val = blob[p + 8:p + 8 + tsize]
        tlv.append((tid, tsize, val))
        p += 8 + tsize
    out["tlv"] = tlv

    # Descifrado y tabla de items
    plain = pkg_crypt(blob[0x60:0x70], blob[data_off:data_off + data_size])

    entries = []
    for i in range(item_count):
        base = i * ITEM_ENTRY_SIZE
        name_off, name_len, d_off, d_size, flags, _pad = struct.unpack_from(
            ">IIQQII", plain, base)
        name = plain[name_off:name_off + name_len].decode("ascii", "replace")
        entries.append({
            "name": name,
            "offset": d_off,
            "size": d_size,
            "flags": flags,
            "kind": flags & 0xFF,
        })
    out["entries"] = entries
    out["plain"] = plain
    return out


def cmd_verify(path):
    info = parse_pkg(path)
    print("Fichero        : %s" % path)
    print("Tamanio        : %d bytes" % info["file_size"])
    print("Magic          : 0x%08X (.PKG)" % PKG_MAGIC)
    print("pkg_revision   : 0x%04X (%s)"
          % (info["revision"],
             "FINALIZADO/retail" if info["finalized"] else "debug/no finalizado"))
    print("pkg_type       : 0x%04X (%s)"
          % (info["pkg_type"],
             "PS3" if info["pkg_type"] == 1 else "PSP/PS Vita"))
    print("metadata_offset: 0x%X  count=%d  size=0x%X"
          % (info["metadata_offset"], info["metadata_count"],
             info["metadata_size"]))
    print("item_count     : %d" % info["item_count"])
    print("total_size     : 0x%X" % info["total_size"])
    print("data_offset    : 0x%X" % info["data_offset"])
    print("data_size      : 0x%X" % info["data_size"])
    print("ContentID      : %s" % info["content_id"])
    print("digest (0x60)  : %s" % info["digest"].hex())
    print("pkg_data_riv   : %s" % info["riv"].hex())
    print()
    print("Metadatos TLV:")
    for tid, tsize, val in info["tlv"]:
        print("  0x%02X  size=%d  %s" % (tid, tsize, val.hex()))
    print()
    print("Items:")
    for e in info["entries"]:
        kind = {F_NPDRMSELF: "NPDRM SELF", F_EDAT: "EDAT",
                F_RAW: "raw", F_DIRECTORY: "dir"}.get(e["kind"], "?")
        ow = "+" if e["flags"] & F_OVERWRITE else "-"
        print("  %s%s  %10d  off=0x%06X  %s"
              % (ow, kind.ljust(10), e["size"], e["offset"], e["name"]))
    print()
    if info["problems"]:
        print("PROBLEMAS:")
        for p in info["problems"]:
            print("  - %s" % p)
        return 1
    print("Estructura y digests: OK")
    return 0


def cmd_extract(path, dest):
    info = parse_pkg(path)
    order = [e for e in info["entries"] if e["kind"] == F_DIRECTORY] + \
            [e for e in info["entries"] if e["kind"] != F_DIRECTORY]
    for e in order:
        name = e["name"]
        while name.startswith("../"):
            name = name[3:]
        name = name.lstrip("/")
        target = os.path.join(dest, name.replace("/", os.sep))
        if e["kind"] == F_DIRECTORY:
            os.makedirs(target, exist_ok=True)
            continue
        os.makedirs(os.path.dirname(target) or dest, exist_ok=True)
        data = info["plain"][e["offset"]:e["offset"] + e["size"]]
        with open(target, "wb") as fp:
            fp.write(data)
        print("  %8d  %s" % (len(data), name))
    print("Extraidos %d items en %s" % (len(order), dest))
    return 0


# ----------------------------------------------------------------------------
# CLI
# ----------------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Generador de .pkg de PS3 (debug/no finalizado)")
    sub = ap.add_subparsers(dest="cmd")

    b = sub.add_parser("build", help="crear un .pkg")
    b.add_argument("folder")
    b.add_argument("output")
    b.add_argument("-c", "--content-id", default=None)
    b.add_argument("-t", "--content-type", default="gameexec",
                   choices=["gameexec", "gamedata"])
    b.add_argument("--drm", default="free",
                   choices=["free", "local", "network"])
    b.add_argument("--package-flags", default=None,
                   help="p.ej. 0x0E (normal) o 0x4E")
    b.add_argument("-r", "--root", default=None,
                   help="ruta de instalacion absoluta, p.ej. "
                        "/dev_hdd0/GAMES/BLUS30016")
    b.add_argument("-v", "--verbose", action="store_true")

    v = sub.add_parser("verify", help="comprobar un .pkg")
    v.add_argument("pkg")

    e = sub.add_parser("extract", help="extraer un .pkg")
    e.add_argument("pkg")
    e.add_argument("dest")

    args = ap.parse_args(argv)

    if args.cmd == "build":
        cid = args.content_id
        if cid is None:
            cid = guess_content_id(args.folder)
        flags = (int(args.package_flags, 0) if args.package_flags
                 else PT_NORMAL)
        res = build_pkg(
            args.folder, args.output, cid,
            content_type=CT_GAMEDATA if args.content_type == "gamedata"
            else CT_GAMEEXEC,
            package_flags=flags,
            drm_type={"free": DRM_FREE, "local": DRM_LOCAL,
                      "network": DRM_NETWORK}[args.drm],
            absolute_root=args.root,
            verbose=args.verbose,
        )
        print("Creado   : %s" % res["path"])
        print("ContentID: %s" % res["content_id"])
        print("Items    : %d (%d ficheros)" % (res["items"], res["files"]))
        print("Datos    : %d bytes (cifrados)" % res["data_size"])
        print("Total    : %d bytes" % res["total_size"])
        return 0

    if args.cmd == "verify":
        return cmd_verify(args.pkg)

    if args.cmd == "extract":
        return cmd_extract(args.pkg, args.dest)

    ap.print_help()
    return 2


def guess_content_id(folder):
    tid = None
    sfo = os.path.join(folder, "PARAM.SFO")
    if os.path.isfile(sfo):
        with open(sfo, "rb") as fp:
            data = fp.read()
        try:
            tid = read_sfo_value(data, "TITLE_ID")
        except Exception:
            tid = None
    if not tid:
        raise SystemExit(
            "No se encontro TITLE_ID en PARAM.SFO; pasa -c/--content-id "
            "(p.ej. UP0001-NPEB00000_00-0000000000000000)")
    return "UP0001-%s_00-0000000000000000" % tid


def read_sfo_value(data, key):
    """Lector minimo de PARAM.SFO."""
    if data[0:4] != b"\x00PSF":
        raise ValueError("no es un PARAM.SFO")
    key_start, data_start, count = struct.unpack_from(">III", data, 8)
    keyb = key.encode("ascii")
    for i in range(count):
        off = 0x14 + i * 0x10
        koff, fmt, ln, _max, doff = struct.unpack_from(">HHIII", data, off)
        k = data[key_start + koff:data[key_start + koff + ln]].split(b"\x00")[0]
        if k == keyb:
            if fmt == 0x0404:
                raw = data[data_start + doff:data_start + doff + ln]
                return raw.split(b"\x00")[0].decode("utf-8", "replace")
            if fmt == 0x0404 or fmt == 0x0004:
                return struct.unpack_from(">I", data, data_start + doff)[0]
    raise KeyError(key)


if __name__ == "__main__":
    sys.exit(main())
