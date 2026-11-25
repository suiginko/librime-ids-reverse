#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
dump_ids4.py — IDS4 二进制数据表 (ids_data.bin) 反解与文本导出工具
"""

import sys
import struct
import os

BLOCK_NAMES = [
    "基本", "扩A", "扩B", "扩C", "扩D", "扩E", "扩F", "扩G", "扩H", "扩I", "扩J",
    "兼容", "兼容补充", "部首补充", "康熙部首", "笔画", "未知"
]

SECTION_NAMES = {
    1: "CP2ID", 2: "ID2CP", 3: "CHAR", 4: "COLD_IDX", 5: "ALTS",
    6: "LEAVES", 7: "ATOMS", 8: "COMPSET", 9: "COLD", 10: "TOKENS",
    11: "PY", 12: "KEY_LOOKUP", 13: "KEY_REC", 14: "POST",
    15: "VAR_OFF", 16: "VAR", 17: "TOK_CANON", 18: "ATOMIC_SET",
    19: "LEAF_STRUCT", 20: "PY_CHAR",
}


def dump_ids4(bin_path, output_path=None, sample_count=None):
    if not os.path.exists(bin_path):
        print(f"Error: {bin_path} does not exist.")
        return

    with open(bin_path, 'rb') as f:
        blob = f.read()

    header = blob[:32]
    magic, ver, flags, max_cp, num_chars, num_keys, num_sec = struct.unpack(
        '<4sHHIIII', header[:24])
    if magic != b'IDS4':
        print(f"Error: Invalid magic {magic}, expected IDS4")
        return
    if ver != 3:
        print(f"Error: Unsupported IDS4 version {ver}, expected 3; "
              f"please regenerate ids_data.bin")
        return

    print("=== IDS4 Binary Data Overview ===")
    print(f"File Size: {len(blob) / 1024 / 1024:.2f} MB")
    print(f"Magic: {magic.decode('ascii')} (Version {ver})")
    print(f"Total Characters: {num_chars}")
    print(f"Max Codepoint: 0x{max_cp:X}")
    print(f"Key Records: {num_keys}")
    print(f"Sections: {num_sec}")

    sections = {}
    st = 32
    for i in range(num_sec):
        sec_type, pad, off, length = struct.unpack(
            '<IIQQ', blob[st + i * 24: st + (i + 1) * 24])
        sections[sec_type] = (off, length)
        name = SECTION_NAMES.get(sec_type, str(sec_type))
        print(f"  [{name:12s}] off={off:>9} len={length:>10}")

    if 19 in sections:
        off, length = sections[19]
        ls_count = struct.unpack('<I', blob[off:off + 4])[0]
        hashes = set()
        for i in range(ls_count):
            h = struct.unpack('<I', blob[off + 4 + i * 8:off + 8 + i * 8])[0]
            hashes.add(h)
        print(f"  LEAF_STRUCT: {ls_count} records, {len(hashes)} unique hashes")

    if 20 not in sections:
        print("Error: missing required PY_CHAR section")
        return
    off, length = sections[20]
    py_count = struct.unpack('<H', blob[off:off + 2])[0]
    p = off + 2
    py_entries = 0
    sha_chars = []
    for _ in range(py_count):
        py_len = struct.unpack('<H', blob[p:p + 2])[0]
        p += 2
        py = blob[p:p + py_len].decode('utf-8')
        p += py_len
        n = struct.unpack('<H', blob[p:p + 2])[0]
        p += 2
        ids = struct.unpack('<' + 'I' * n, blob[p:p + n * 4]) if n else ()
        p += n * 4
        py_entries += n
        if py == 'sha':
            sha_chars = list(ids)
    print(f"  PY_CHAR: {py_count} pinyin keys, {py_entries} char entries; "
          f"sha={len(sha_chars)}")

    id2cp_off, _ = sections[2]
    char_off, _ = sections[3]
    cold_idx_off, _ = sections[4]
    cold_off, cold_len = sections[9]

    id2cp = struct.unpack(
        f'<{num_chars}I', blob[id2cp_off: id2cp_off + num_chars * 4])

    count_to_export = min(num_chars, sample_count) if sample_count else num_chars

    lines = []
    lines.append("# 汉字\tUnicode\t读音\tIDS结构\t分区\t拆分深度\t字频权重")

    for cid in range(count_to_export):
        cp = id2cp[cid]
        char_str = chr(cp)

        # CHAR: freq u32, flags u32, char_token u16, depth u8, block u8,
        #       alts_off u32, leaf_off u32, atom_off u32 (24 字节)
        h_pos = char_off + cid * 24
        freq, flags, char_token, depth, block_id, alts_off, leaf_off, atom_off = \
            struct.unpack('<IIHBBIII', blob[h_pos: h_pos + 24])
        block_name = BLOCK_NAMES[block_id] if block_id < len(BLOCK_NAMES) else "未知"

        # COLD_IDX: cs_off u32, cold_off u32
        ci_pos = cold_idx_off + cid * 8
        cs_off, c_off = struct.unpack('<II', blob[ci_pos: ci_pos + 8])

        # COLD: py_len u16, py bytes, ids_len u16, ids bytes
        c_pos = cold_off + c_off
        if c_pos + 2 > cold_off + cold_len:
            continue
        py_len = struct.unpack('<H', blob[c_pos: c_pos + 2])[0]
        py_str = blob[c_pos + 2: c_pos + 2 + py_len].decode('utf-8', errors='replace')
        p_ids = c_pos + 2 + py_len
        ids_len = struct.unpack('<H', blob[p_ids: p_ids + 2])[0]
        ids_str = blob[p_ids + 2: p_ids + 2 + ids_len].decode('utf-8', errors='replace')

        line = f"{char_str}\tU+{cp:04X}\t{py_str}\t{ids_str}\t{block_name}\t{depth}\t{freq}"
        lines.append(line)

    # 异体字 section (15: VAR_OFF, 16: VAR)，存在则反解导出
    if 15 in sections and 16 in sections:
        var_off_off, _ = sections[15]
        var_off, var_len = sections[16]
        var_offsets = struct.unpack(
            f'<{num_chars}I', blob[var_off_off: var_off_off + num_chars * 4])
        var_lines = []
        for vid in range(num_chars):
            p = var_off + var_offsets[vid]
            if p + 2 > var_off + var_len:
                break
            cnt = struct.unpack('<H', blob[p: p + 2])[0]
            if cnt == 0:
                continue
            variants = struct.unpack(f'<{cnt}I', blob[p + 2: p + 2 + cnt * 4])
            vchars = ' '.join(chr(id2cp[v]) for v in variants if v < num_chars)
            var_lines.append(f"{chr(id2cp[vid])}\t{vchars}")
        vout = "ids_variants_dumped.txt"
        with open(vout, 'w', encoding='utf-8') as out:
            out.write("# 原字\t异体字列表 (空格分隔)\n" + '\n'.join(var_lines) + '\n')
        print(f"[Variants] {len(var_lines)} base chars exported to {vout}")

    if output_path:
        with open(output_path, 'w', encoding='utf-8') as out:
            out.write('\n'.join(lines) + '\n')
        print(f"\n[Success] Exported {count_to_export} characters to: {output_path}")
        print(f"Output File Size: {os.path.getsize(output_path) / 1024 / 1024:.2f} MB")
    else:
        print("\n--- Preview (First 15 characters) ---")
        for l in lines[:16]:
            print(l)


if __name__ == '__main__':
    bin_file = sys.argv[1] if len(sys.argv) > 1 else 'ids_data.bin'
    out_file = sys.argv[2] if len(sys.argv) > 2 else 'ids_data_dumped.txt'
    dump_ids4(bin_file, out_file)
