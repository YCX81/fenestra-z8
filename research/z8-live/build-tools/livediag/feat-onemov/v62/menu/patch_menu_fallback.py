#!/usr/bin/env python3
"""Bounded NFP text fallback after a Fenestra legacy UI text miss.

Post-composition patch: exactly three AArch64 instructions; no build ID or
packaging changes. Caller must bind the input SHA and use a fresh output.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

BASE = 0x40160000
SITE = 0x401CD4C8
NFP_FIRST, NFP_LAST = 3180, 3224
LANGUAGES, NATIVE_COUNT = 36, 3225
TABLE_DIRECTORY, POOL_DIRECTORY = 0x4327C5A0, 0x439DAF58
NATIVE_FALLBACK, EMPTY_FALLBACK = 0x401CD524, 0x401CD45C
GUARDS = {
    0x401CD49C: 'a8b231511f250171c8010054a00100b400008012d9feff97e103152ae20314aae30313aa6fbcff97c01400351f2003d568ea09107f0200b9880200f9a1000014',
    EMPTY_FALLBACK: '48020a507f0200b9880200f9a01700b4',
    NATIVE_FALLBACK: '88ab99d2098795d22850a0f2495ea0f24801c0f24901c0f2080140b92a0940f91f01156b48d976f8290140f9aac29f1a08c92a8b29d976f80a2140294a7d409329010a8b08010a4b890200f9680200b97a000014',
    0x401CD414: 'b7da32712b040054',
    0x411D9D2C: '22938152',  # native loader count = 3225
    0x411D9D6C: '22938152',
}


def need(ok, message):
    if not ok:
        raise ValueError(message)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def branch(at, target, condition=None):
    distance = target - at
    need(distance % 4 == 0, 'unaligned branch')
    if condition is None:
        need(-(1 << 27) <= distance < (1 << 27), 'B out of range')
        word = 0x14000000 | ((distance // 4) & 0x3FFFFFF)
    else:
        need(-(1 << 20) <= distance < (1 << 20), 'B.cond out of range')
        word = 0x54000000 | (((distance // 4) & 0x7FFFF) << 5) | condition
    return struct.pack('<I', word)


def replacement():
    # At this point the existing range check proved id in [3180,3253], and
    # fx_native_ui_text returned false. w21=id, w22=language are preserved.
    cmp_w21 = struct.pack('<I', 0x7100001F | (NFP_LAST << 10) | (21 << 5))
    return cmp_w21 + branch(SITE + 4, NATIVE_FALLBACK, 9) + branch(SITE + 8, EMPTY_FALLBACK)


def native_text(image, language, ident):
    table = struct.unpack_from('<Q', image, TABLE_DIRECTORY - BASE + 8 * language)[0]
    origin = struct.unpack_from('<Q', image, POOL_DIRECTORY - BASE + 8 * language)[0]
    begin, end = struct.unpack_from('<2i', image, table - BASE + 4 * ident)
    offset = origin + begin - BASE
    need(0 <= offset <= len(image) and 0 <= end - begin <= 4096 and offset + end - begin <= len(image),
         f'bad native text bounds lang={language} id={ident}')
    need((end - begin) % 2 == 0, 'non UTF-16 text length')
    return image[offset:offset + end - begin]


def validate(image):
    need(len(image) == 81807709, 'unexpected MAIN size')
    for address, expected in GUARDS.items():
        expected = bytes.fromhex(expected)
        need(image[address - BASE:address - BASE + len(expected)] == expected,
             f'guard differs at {address:#x}')
    tables = struct.unpack_from('<36Q', image, TABLE_DIRECTORY - BASE)
    stride = (NATIVE_COUNT + 1) * 4
    need(all(t == tables[0] + stride * i for i, t in enumerate(tables)),
         'native table stride does not establish count 3225 for all languages')
    texts = []
    for language in range(LANGUAGES):
        for ident in range(NFP_FIRST, NFP_LAST + 1):
            data = native_text(image, language, ident)
            need(bool(data), f'empty NFP source lang={language} id={ident}')
            data.decode('utf-16le', errors='strict')
            texts.append(data)
    need(native_text(image, 8, 3218).decode('utf-16le') == 'Log设定', 'zh-CN title differs')
    need(native_text(image, 8, 3219).decode('utf-16le') == 'N-Log / BT.2020', 'N-Log label differs')
    need(native_text(image, 8, 3220).decode('utf-16le') == 'S-Log3 / S-Gamut3.Cine', 'S-Log3 label differs')
    need(native_text(image, 8, 3221).decode('utf-16le') == 'Log3G10 / REDWideGamutRGB', 'Log3G10 label differs')
    return {'languages': LANGUAGES, 'native_count': NATIVE_COUNT,
            'restored_id_range': [NFP_FIRST, NFP_LAST], 'native_texts_verified': len(texts),
            'native_texts_sha256': sha(b''.join(texts))}


def patch(image, expected_sha256):
    need(sha(image) == expected_sha256, 'input SHA256 mismatch')
    source = validate(image)
    changed = bytearray(image)
    changed[SITE - BASE:SITE - BASE + 12] = replacement()
    need(changed[:SITE - BASE] == image[:SITE - BASE] and changed[SITE - BASE + 12:] == image[SITE - BASE + 12:],
         'unexpected modification outside the 12-byte instruction window')
    return bytes(changed), source


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--input', type=Path, required=True)
    ap.add_argument('--expected-sha256', required=True)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--receipt', type=Path, required=True)
    a = ap.parse_args()
    need(not a.output.exists() and not a.receipt.exists(), 'output or receipt exists')
    original = a.input.read_bytes()
    result, source = patch(original, a.expected_sha256)
    a.output.parent.mkdir(parents=True, exist_ok=True)
    a.output.write_bytes(result)
    record = {'input': str(a.input), 'input_sha256': sha(original), 'output': str(a.output),
              'output_sha256': sha(result), 'instruction_window': {'address': hex(SITE), 'bytes': 12,
              'before': original[SITE - BASE:SITE - BASE + 12].hex(), 'after': replacement().hex()},
              'changed_byte_count': sum(x != y for x, y in zip(original, result)),
              'source_guard': source, 'build_id_recalculated': False,
              'semantics': 'Preserve successful dynamic UI text (including empty); on miss resolve NFP 3180..3224 through native language tables; higher legacy UI IDs remain empty.'}
    a.receipt.parent.mkdir(parents=True, exist_ok=True)
    a.receipt.write_text(json.dumps(record, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps(record, ensure_ascii=False))


if __name__ == '__main__':
    main()
