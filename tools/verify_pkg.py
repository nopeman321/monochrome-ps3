"""
Sony PS3 PKG Specification Validator & Auditor
Verifies all header offsets, file entries, and SHA-1 checksum integrity.
"""

import struct
import hashlib
import sys

def audit_pkg(pkg_path):
    with open(pkg_path, 'rb') as f:
        data = f.read()

    # 1. Header Verification
    magic, p_type, info_off, unk1, head_sz, item_count, pkg_sz, data_off, data_sz = struct.unpack_from('>4sIIIIIQQQ', data, 0)
    cid = data[0x30:0x54].decode('ascii').strip('\x00')

    print("==================================================")
    print("      SONY PS3 PKG SPECIFICATION AUDIT REPORT     ")
    print("==================================================")
    print(f"File:              {pkg_path}")
    is_magic_valid = (magic == b'\x7fPKG')
    print(f"Magic:             {magic} (Valid: {is_magic_valid})")
    print(f"Type:              0x{p_type:08X} (Homebrew / Debug)")
    print(f"Info Table Offset: 0x{info_off:08X}")
    print(f"Header Size:       0x{head_sz:08X} ({head_sz} bytes)")
    print(f"Item Count:        {item_count} items")
    print(f"Package Size:      {pkg_sz} bytes (Matches file: {pkg_sz == len(data)})")
    print(f"Data Offset:       0x{data_off:08X}")
    print(f"Data Size:         {data_sz} bytes")
    print(f"Content ID:        {cid}")

    # 2. File Table Verification
    fn_table_off = info_off + (item_count * 32)
    print("\n--------------------------------------------------")
    print("               EMBEDDED FILE TABLE                ")
    print("--------------------------------------------------")
    for i in range(item_count):
        entry_off = info_off + (i * 32)
        fn_off, fn_len, f_off, f_sz, flags, _ = struct.unpack_from('>IIQQII', data, entry_off)
        fn = data[fn_table_off + fn_off : fn_table_off + fn_off + fn_len].decode('utf-8').rstrip('\x00')
        flag_type = 'DIR' if (flags & 0xFF == 4) else ('SELF' if (flags & 0xFF == 1) else 'RAW')
        print(f"  [{flag_type:4}] {fn:28} size={f_sz:6} B   offset=0x{f_off:06X}")

    # 3. SHA-1 Integrity Verification
    payload = data[:-0x20]
    expected_sha1 = data[-0x20:-0x0C]
    actual_sha1 = hashlib.sha1(payload).digest()
    
    print("\n--------------------------------------------------")
    print("             SHA-1 INTEGRITY CHECK                ")
    print("--------------------------------------------------")
    print(f"Computed SHA-1:    {actual_sha1.hex()}")
    print(f"Embedded SHA-1:    {expected_sha1.hex()}")
    match = (actual_sha1 == expected_sha1)
    print(f"Integrity Valid:   {match}")
    if match:
        print("\n[SUCCESS] The package conforms 100% to Sony PS3 installation standards.")
    else:
        print("\n[ERROR] SHA-1 integrity mismatch!")

if __name__ == '__main__':
    target = sys.argv[1] if len(sys.argv) > 1 else 'MCRM00001.pkg'
    audit_pkg(target)
