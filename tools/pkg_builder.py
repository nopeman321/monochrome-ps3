"""
PS3 Package (PKG) Builder for Windows and Linux
Generates installable .pkg files for PlayStation 3 (CFW, PS3HEN, RPCS3).
Implements Sony Debug PKG specification with SHA-1 keystream stream cipher encryption,
fully compatible with RPCS3 Package Installer, PS3HEN, and CFW Package Manager.

Header layout (0x80 bytes, Big Endian):
  0x00: magic (4 bytes) -> 0x7F 'P' 'K' 'G'
  0x04: pkg_type (2 bytes) -> 0x0000 (Debug)
  0x06: pkg_platform (2 bytes) -> 0x0001 (PS3)
  0x08: meta_offset (4 bytes) -> 0x00000080
  0x0C: meta_count (4 bytes) -> 0x00000000
  0x10: meta_size (4 bytes) -> 0x00000080
  0x14: file_count (4 bytes) -> number of entries
  0x18: pkg_size (8 bytes) -> total package size in bytes
  0x20: data_offset (8 bytes) -> offset where data payload begins (0x00000080)
  0x28: data_size (8 bytes) -> total size of data payload
  0x30: content_id (48 bytes: 36 ASCII + 12 null padding)
  0x60: qa_digest (16 bytes)
  0x70: klicensee (16 bytes)

PKGEntry layout (32 bytes per entry, Big Endian):
  0x00: name_offset (4 bytes) -> offset of filename within payload
  0x04: name_size (4 bytes) -> length of filename string including null terminator
  0x08: file_offset (8 bytes) -> offset of file data within payload (0 for dir)
  0x10: file_size (8 bytes) -> size of file (0 for dir)
  0x18: type (4 bytes) -> 0x80000001 (NPDRM), 0x80000003 (RAW), 0x80000004 (DIR)
  0x1C: pad (4 bytes) -> 0x00000000
"""

import os
import sys
import struct
import hashlib

PKG_MAGIC = b"\x7fPKG"

# File entry types
TYPE_NPDRMSELF = 0x01
TYPE_RAW       = 0x03
TYPE_DIRECTORY = 0x04
FLAG_OVERWRITE = 0x80000000

def encrypt_debug(data_bytes, qa_digest_bytes):
    """
    Encrypts/decrypts the Debug PKG payload using Sony's SHA-1 keystream cipher.
    Cipher block:
      be_t<u64> input[8] = { qa[0], qa[0], qa[1], qa[1], 0, 0, 0, block_index };
      hash = SHA1(input);
      ciphertext = plaintext ^ hash[0..15];
    """
    qa_0, qa_1 = struct.unpack('>QQ', qa_digest_bytes)
    blocks = (len(data_bytes) + 15) // 16
    encrypted = bytearray(data_bytes)
    for i in range(blocks):
        inp = struct.pack('>QQQQQQQQ', qa_0, qa_0, qa_1, qa_1, 0, 0, 0, i)
        h = hashlib.sha1(inp).digest()[:16]
        chunk_len = min(16, len(data_bytes) - i * 16)
        for j in range(chunk_len):
            encrypted[i * 16 + j] ^= h[j]
    return bytes(encrypted)

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
    print(f"[+] Building PS3 Debug PKG from: {source_dir}")
    print(f"[+] Content-ID: {content_id}")
    
    items = collect_files(source_dir)
    print(f"[+] Total package items: {len(items)}")

    entry_count = len(items)
    entry_table_size = entry_count * 32

    # 1. Build Filename Table
    filename_table = bytearray()
    file_info = []

    for rel_path, abs_path, is_dir, size in items:
        fn_bytes = rel_path.encode('utf-8') + b'\x00'
        fn_offset = entry_table_size + len(filename_table)
        filename_table.extend(fn_bytes)
        # 4-byte align filename table entries
        while len(filename_table) % 4 != 0:
            filename_table.append(0)

        if is_dir:
            entry_type = TYPE_DIRECTORY | FLAG_OVERWRITE
        elif rel_path.endswith('.BIN') or rel_path.endswith('.self'):
            entry_type = TYPE_NPDRMSELF | FLAG_OVERWRITE
        else:
            entry_type = TYPE_RAW | FLAG_OVERWRITE

        file_info.append((fn_offset, len(fn_bytes), abs_path, is_dir, size, entry_type))

    # Pad filename table to 16-byte boundary before file data begins
    while (entry_table_size + len(filename_table)) % 16 != 0:
        filename_table.append(0)

    data_start_rel = entry_table_size + len(filename_table)

    # 2. Build File Data Stream & Entry Records
    file_entries = []
    file_data_stream = bytearray()

    for fn_off, fn_len, abs_path, is_dir, size, entry_type in file_info:
        if is_dir:
            file_entries.append((fn_off, fn_len, 0, 0, entry_type))
        else:
            file_off = data_start_rel + len(file_data_stream)
            with open(abs_path, 'rb') as f:
                content = f.read()
            file_data_stream.extend(content)
            # 16-byte align each file within payload
            while len(file_data_stream) % 16 != 0:
                file_data_stream.append(0)
            file_entries.append((fn_off, fn_len, file_off, size, entry_type))

    # 3. Build Plaintext Entry Table
    entry_table = bytearray()
    for fn_off, fn_len, f_off, f_size, e_type in file_entries:
        entry_table.extend(struct.pack('>IIQQII', fn_off, fn_len, f_off, f_size, e_type, 0))

    # Assemble complete plaintext payload
    plaintext_payload = bytearray(entry_table + filename_table + file_data_stream)
    # Ensure payload ends on 16-byte block boundary
    while len(plaintext_payload) % 16 != 0:
        plaintext_payload.append(0)

    payload_size = len(plaintext_payload)
    data_offset = 0x80  # Directly follows 0x80 header

    # Generate 16-byte QA Digest from Content-ID
    qa_digest = hashlib.sha1(content_id.encode('ascii')).digest()[:16]

    print(f"[+] Encrypting {payload_size} bytes payload with Debug SHA-1 keystream cipher...")
    encrypted_payload = encrypt_debug(plaintext_payload, qa_digest)

    total_pkg_size = data_offset + payload_size + 0x60  # Header + Encrypted Payload + 0x60 Footer

    # 4. Build 0x80-byte Sony PKG Header
    header = bytearray(0x80)
    struct.pack_into('>4sHHIIIIQQQ', header, 0,
        PKG_MAGIC,           # 0x00: \x7fPKG
        0x0000,              # 0x04: pkg_type = 0x0000 (Debug)
        0x0001,              # 0x06: pkg_platform = 0x0001 (PS3)
        0x0080,              # 0x08: meta_offset = 0x80
        0x0000,              # 0x0C: meta_count = 0
        0x0080,              # 0x10: meta_size = 0x80
        entry_count,         # 0x14: file_count
        total_pkg_size,      # 0x18: pkg_size (8 bytes)
        data_offset,         # 0x20: data_offset (8 bytes)
        payload_size         # 0x28: data_size (8 bytes)
    )

    cid_bytes = content_id.encode('ascii').ljust(36, b'\x00')[:36] + (b'\x00' * 12)
    header[0x30:0x60] = cid_bytes
    header[0x60:0x70] = qa_digest
    header[0x70:0x80] = b'\x00' * 16  # KLicensee (zeros for Free NPDRM)

    pkg_data = header + encrypted_payload

    # 5. Compute SHA-1 digest footer (0x60 bytes)
    sha1 = hashlib.sha1(pkg_data).digest()
    footer = sha1.ljust(0x60, b'\x00')

    final_pkg = pkg_data + footer

    os.makedirs(os.path.dirname(os.path.abspath(output_pkg)) or '.', exist_ok=True)
    with open(output_pkg, 'wb') as f:
        f.write(final_pkg)

    print(f"[SUCCESS] PS3 Debug PKG generated ({len(final_pkg)} bytes): {output_pkg}")

if __name__ == '__main__':
    src = sys.argv[1] if len(sys.argv) > 1 else 'pkg_build'
    dst = sys.argv[2] if len(sys.argv) > 2 else 'MCRM00001.pkg'
    build_pkg(src, dst)
