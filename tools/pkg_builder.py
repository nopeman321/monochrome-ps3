"""
PS3 Package (PKG) Builder for Windows and Linux
Generates installable .pkg files for PlayStation 3 (CFW, PS3HEN, RPCS3).
Implements the exact Sony PKG / PSL1GHT specification.

Header layout (0x80 bytes, Big Endian):
  0x00: magic (4 bytes) -> 0x7F 'P' 'K' 'G'
  0x04: type (4 bytes) -> 0x00000001
  0x08: pkgInfoOff (4 bytes) -> offset to FileHeader array
  0x0C: unk1 (4 bytes) -> 0x00000000
  0x10: headSize (4 bytes) -> 0x00000080
  0x14: itemCount (4 bytes) -> number of entries
  0x18: packageSize (8 bytes) -> total package size in bytes
  0x20: dataOff (8 bytes) -> offset where data payload begins
  0x28: dataSize (8 bytes) -> total size of data payload
  0x30: contentID (48 bytes: 36 ASCII + 12 null padding)
  0x60: QADigest (16 bytes)
  0x70: KLicensee (16 bytes)

FileHeader entry layout (32 bytes per entry, Big Endian):
  0x00: fileNameOff (4 bytes) -> offset in filename table
  0x04: fileNameLength (4 bytes) -> length of filename string
  0x08: fileOff (8 bytes) -> offset of file data within payload
  0x10: fileSize (8 bytes) -> size of file
  0x18: flags (4 bytes) -> 0x01 (SELF), 0x03 (RAW), 0x04 (DIR) | 0x80000000 (OVERWRITE)
  0x1C: padding (4 bytes) -> 0x00000000
"""

import os
import sys
import struct
import hashlib

PKG_MAGIC = b"\x7fPKG"

TYPE_NPDRMSELF = 0x01
TYPE_RAW       = 0x03
TYPE_DIRECTORY = 0x04
FLAG_OVERWRITE = 0x80000000

def collect_files(source_dir):
    """Scans the source directory and returns list of (rel_path, abs_path, is_dir, size)."""
    items = []
    for root, dirs, files in os.walk(source_dir):
        rel_root = os.path.relpath(root, source_dir).replace("\\", "/")
        if rel_root != ".":
            items.append((rel_root, root, True, 0))
            
        for f in sorted(files):
            abs_f = os.path.join(root, f)
            rel_f = os.path.relpath(abs_f, source_dir).replace("\\", "/")
            items.append((rel_f, abs_f, False, os.path.getsize(abs_f)))
            
    items.sort(key=lambda x: x[0])
    return items

def build_pkg(source_dir, output_pkg, content_id="UP0001-MCRM00001_00-MONOCHROMEPS3001"):
    print(f"[+] Building PS3 PKG from: {source_dir}")
    print(f"[+] Content-ID: {content_id}")
    
    items = collect_files(source_dir)
    print(f"[+] Total package items: {len(items)}")

    # 1. Build filename table and file data payload
    filename_table = bytearray()
    file_records = []
    data_stream = bytearray()

    for rel_path, abs_path, is_dir, size in items:
        fn_bytes = rel_path.encode('utf-8') + b'\x00'
        fn_offset = len(filename_table)
        filename_table.extend(fn_bytes)
        
        # 4-byte align filename table entries
        while len(filename_table) % 4 != 0:
            filename_table.append(0)

        flags = TYPE_DIRECTORY if is_dir else TYPE_RAW
        # Homebrew on PS3 / RPCS3 uses TYPE_RAW (0x03) so no Sony NPDRM .rap license is required

        if not is_dir:
            file_data_offset = len(data_stream)
            with open(abs_path, 'rb') as f:
                content = f.read()
            data_stream.extend(content)
            # 16-byte align each file in payload
            while len(data_stream) % 16 != 0:
                data_stream.append(0)
            file_records.append((fn_offset, len(fn_bytes), file_data_offset, size, flags))
        else:
            file_records.append((fn_offset, len(fn_bytes), 0, 0, flags))

    # 2. Compute exact header, metadata, and data offsets
    header_size = 0x80
    entry_count = len(file_records)
    entry_table_size = entry_count * 32
    
    meta_offset = header_size
    filename_table_offset = meta_offset + entry_table_size
    data_offset = filename_table_offset + len(filename_table)
    
    # 16-byte align data_offset
    data_offset = (data_offset + 15) & ~15
    filename_padding = data_offset - (filename_table_offset + len(filename_table))
    filename_table.extend(b'\x00' * filename_padding)

    total_data_size = len(data_stream)
    # Total package size includes 0x20 bytes for SHA1 digest footer
    total_package_size = data_offset + total_data_size + 0x20

    # 3. Build Sony / PSL1GHT Specification PKG Header (0x80 bytes)
    header = bytearray(0x80)
    struct.pack_into(">4sIIIIIQQQ", header, 0,
                     PKG_MAGIC,           # 0x00: magic (4 bytes)
                     1,                   # 0x04: type (4 bytes)
                     meta_offset,         # 0x08: pkgInfoOff (4 bytes)
                     0,                   # 0x0C: unk1 (4 bytes)
                     header_size,         # 0x10: headSize (4 bytes = 0x80)
                     entry_count,         # 0x14: itemCount (4 bytes)
                     total_package_size,  # 0x18: packageSize (8 bytes)
                     data_offset,         # 0x20: dataOff (8 bytes)
                     total_data_size)     # 0x28: dataSize (8 bytes)

    cid_bytes = content_id.encode('ascii').ljust(36, b'\x00')[:36] + (b'\x00' * 12)
    header[0x30:0x60] = cid_bytes
    header[0x60:0x70] = b'\x00' * 16  # QA digest
    header[0x70:0x80] = b'\x00' * 16  # KLicensee

    # 4. Build FileHeader table (32 bytes per entry)
    entry_table = bytearray()
    for fn_off, fn_len, f_data_off, f_size, flags in file_records:
        entry_table.extend(struct.pack(">IIQQII",
                                       fn_off,
                                       fn_len,
                                       f_data_off if f_size > 0 else 0,
                                       f_size,
                                       flags | FLAG_OVERWRITE,
                                       0))

    # 5. Assemble package payload
    pkg_payload = header + entry_table + filename_table + data_stream
    
    # 6. Compute SHA-1 digest over the package payload
    sha1 = hashlib.sha1(pkg_payload).digest()
    pkg_final = pkg_payload + sha1.ljust(0x20, b'\x00')

    os.makedirs(os.path.dirname(os.path.abspath(output_pkg)) or '.', exist_ok=True)
    with open(output_pkg, 'wb') as f:
        f.write(pkg_final)

    print(f"[OK] PS3 PKG successfully generated ({len(pkg_final)} bytes): {output_pkg}")

if __name__ == '__main__':
    src = sys.argv[1] if len(sys.argv) > 1 else 'pkg_build'
    dst = sys.argv[2] if len(sys.argv) > 2 else 'MCRM00001.pkg'
    build_pkg(src, dst)
