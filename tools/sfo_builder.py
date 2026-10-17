"""
PARAM.SFO Generator for PlayStation 3
Creates official Sony format System Parameter files.

Sony SFO spec notes:
  - String max_len must match the sizes GameOS expects (e.g. TITLE = 128 bytes).
  - UINT32 values are little-endian in the data table.
  - Keys must be sorted alphabetically.
  - The entire file uses little-endian byte order.
"""

import struct
import sys
import os

FMT_SPECIAL = 0x0004
FMT_STRING  = 0x0204
FMT_UINT32  = 0x0404

# Sony-mandated max_len for well-known keys
SONY_MAX_LENS = {
    'APP_VER':        8,
    'ATTRIBUTE':      4,
    'BOOTABLE':       4,
    'CATEGORY':       4,
    'PARENTAL_LEVEL': 4,
    'PS3_SYSTEM_VER': 8,
    'RESOLUTION':     4,
    'SOUND_FORMAT':   4,
    'TITLE':        128,
    'TITLE_ID':      16,
    'VERSION':        8,
}

def create_sfo(entries):
    sorted_keys = sorted(entries.keys())
    count = len(sorted_keys)

    header_size = 20
    index_table_size = count * 16

    # Pass 1: build key table
    key_table = bytearray()
    key_offsets = {}
    for k in sorted_keys:
        key_offsets[k] = len(key_table)
        key_table.extend(k.encode('utf-8') + b'\x00')

    # Pad key table to 4-byte boundary
    while len(key_table) % 4 != 0:
        key_table.append(0)

    key_table_offset = header_size + index_table_size
    data_table_offset = key_table_offset + len(key_table)

    # Pass 2: build data table and index entries
    data_table = bytearray()
    index_table = bytearray()

    for k in sorted_keys:
        fmt, val = entries[k]
        val_offset = len(data_table)

        if fmt == FMT_UINT32:
            data_bytes = struct.pack('<I', val)
            data_len = 4
            max_len = 4
        elif fmt in (FMT_STRING, FMT_SPECIAL):
            raw = val.encode('utf-8') + b'\x00'
            data_len = len(raw)
            # Use Sony-mandated max_len if known, else round up to 4
            max_len = SONY_MAX_LENS.get(k, (data_len + 3) & ~3)
            if max_len < data_len:
                max_len = (data_len + 3) & ~3
            data_bytes = raw.ljust(max_len, b'\x00')
        else:
            raise ValueError(f"Unknown format {fmt}")

        data_table.extend(data_bytes)

        index_table.extend(struct.pack('<HHIII',
            key_offsets[k], fmt, data_len, max_len, val_offset))

    header = struct.pack('<4sIIII',
        b'\x00PSF', 0x00000101, key_table_offset, data_table_offset, count)

    return header + index_table + key_table + data_table


def build_monochrome_sfo(output_path):
    entries = {
        'APP_VER':        (FMT_STRING, '01.00'),
        'BOOTABLE':       (FMT_UINT32, 1),
        'CATEGORY':       (FMT_STRING, 'HG'), # 'HG' = PS3 Harddisk Application
        'PARENTAL_LEVEL': (FMT_UINT32, 0),
        'PS3_SYSTEM_VER': (FMT_STRING, '04.21'),
        'RESOLUTION':     (FMT_UINT32, 63),   # 480p + 576p + 720p + 1080i + 1080p
        'SOUND_FORMAT':   (FMT_UINT32, 1),    # LPCM 2ch
        'TITLE':          (FMT_STRING, 'Monochrome Music'),
        'TITLE_ID':       (FMT_STRING, 'MCRM00001'),
        'VERSION':        (FMT_STRING, '01.00'),
    }

    sfo_data = create_sfo(entries)
    os.makedirs(os.path.dirname(os.path.abspath(output_path)), exist_ok=True)
    with open(output_path, 'wb') as f:
        f.write(sfo_data)
    print(f"[SFO] PARAM.SFO ({len(sfo_data)} bytes) -> {output_path}")


if __name__ == '__main__':
    out = sys.argv[1] if len(sys.argv) > 1 else 'PARAM.SFO'
    build_monochrome_sfo(out)
