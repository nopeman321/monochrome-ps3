import struct
import hashlib
import sys

def decrypt_debug(data_bytes, qa_digest_bytes, offset):
    qa_0, qa_1 = struct.unpack('>QQ', qa_digest_bytes)
    blocks = (len(data_bytes) + 15) // 16
    decrypted = bytearray(data_bytes)
    for i in range(blocks):
        inp = struct.pack('>QQQQQQQQ', qa_0, qa_0, qa_1, qa_1, 0, 0, 0, (offset // 16) + i)
        h = hashlib.sha1(inp).digest()[:16]
        chunk_len = min(16, len(data_bytes) - i * 16)
        for j in range(chunk_len):
            decrypted[i * 16 + j] ^= h[j]
    return bytes(decrypted)

def verify(pkg_path):
    with open(pkg_path, 'rb') as f:
        pkg = f.read()

    magic, p_type, platform, meta_off, meta_cnt, meta_sz, item_cnt, pkg_sz, data_off, data_sz = struct.unpack('>4sHHIIIIQQQ', pkg[:0x30])
    print(f"Magic: {magic}, Type: {p_type}, Platform: {platform}, Items: {item_cnt}, Size: {pkg_sz}, DataOff: {data_off}, DataSz: {data_sz}")

    title_id = pkg[0x30:0x54].rstrip(b'\x00').decode('ascii')
    qa_digest = pkg[0x60:0x70]
    print(f"TitleID: {title_id}, QADigest: {qa_digest.hex()}")

    # Read entries
    encrypted_entries = pkg[data_off : data_off + item_cnt * 32]
    dec_entries = decrypt_debug(encrypted_entries, qa_digest, 0)

    fsz = len(pkg) - data_off
    print(f"Available payload fsz: {fsz}")
    for i in range(item_cnt):
        name_off, name_sz, f_off, f_sz, e_type, pad = struct.unpack('>IIQQII', dec_entries[i*32:(i+1)*32])
        print(f"Entry {i}: name_off={name_off}, name_sz={name_sz}, f_off={f_off}, f_sz={f_sz}, type={hex(e_type)}, pad={pad}")
        
        assert name_sz <= 256, f"name_sz too big: {name_sz}"
        assert fsz >= name_sz and (fsz - name_sz) >= name_off, "name exceeds bounds"
        if f_sz > 0:
            assert fsz >= f_sz and (fsz - f_sz) >= f_off, "file exceeds bounds"
            assert name_off != f_off, "name_off == f_off"

        # decrypt name
        # align to 16
        name_block_start = name_off & ~15
        name_block_end = (name_off + name_sz + 15) & ~15
        enc_name_block = pkg[data_off + name_block_start : data_off + name_block_end]
        dec_name_block = decrypt_debug(enc_name_block, qa_digest, name_block_start)
        name_bytes = dec_name_block[name_off - name_block_start : name_off - name_block_start + name_sz]
        name_str = name_bytes.rstrip(b'\x00').decode('utf-8')
        print(f"   Name: '{name_str}'")

    print("[SUCCESS] All RPCS3 PKG checks passed 100%!")

if __name__ == '__main__':
    verify(sys.argv[1] if len(sys.argv) > 1 else 'test_debug.pkg')
