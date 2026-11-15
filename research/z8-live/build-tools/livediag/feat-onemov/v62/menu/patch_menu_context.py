#!/usr/bin/env python3
"""Context-qualified native/NFP text ownership with original UI semantics.

Input is the pre-menu composed MAIN. Build ID remains the caller's responsibility.
"""
import argparse
import json
from pathlib import Path
import struct

import patch_menu_fallback as miss

BASE = miss.BASE
HOOK = 0x401CD410
HELPER = 0x401FCF40
RESERVED = 0xB0
PRIMARY = 0x401E2000
LIMIT = 0x401FD000
HELPER_BYTES = bytes.fromhex(
    'a8b231511fb10071689ee754fe0f1ff8997b0794800100b4080040b9093d0053'
    '3f09007101010054087d1053081d405109a084521f01096b62000054fe0741f8'
    'e53cff17e0031faafe0741f8c0035fd6')
GUARDS = {
    0x401CD3F0: 'f70b00f9fd030091f65702a9f503012af603002af44f03a9f30303aaf40302aac1fbff97b7da3271',
    0x403DBDB4: 'fe0f1ff8082c8ad26865a0f24801c0f20800e0f2000140f931370094400000b4000040f9fe0741f8c0035fd6',
    0x403E9A90: '000100b4080840b9e8000034090040b9080500510a0840f9287d081b4001088bc0035fd6e0031faac0035fd6',
    0x403ADAA4: 'e023009153b80094e803002a',
    0x403DBC6C: 'c00240f9e10313aa2b370094600240f9acfeff97',
    0x403ADBF0: 'c9010094e00740f9e1030032be000094e00740f915950094e00740f9e10300329f0e0094',
}


def patch(image, expected_sha256, build):
    miss.need(miss.sha(image) == expected_sha256, 'input SHA256 mismatch')
    tables = miss.validate(image)
    for address, expected in GUARDS.items():
        expected = bytes.fromhex(expected)
        miss.need(image[address - BASE:address - BASE + len(expected)] == expected,
                  f'context guard differs at {address:#x}')
    payload = build.get('live_payload_bytes')
    miss.need(type(payload) is int and payload > 0, 'missing primary live payload bound')
    end = PRIMARY + payload
    miss.need(end <= HELPER and build.get('live_free') == LIMIT - end,
              'primary live payload overlaps reserved menu context helper')
    miss.need(not any(image[end - BASE:HELPER + RESERVED - BASE]),
              'primary tail or context reservation is not zero')
    miss.need(len(HELPER_BYTES) <= RESERVED and HELPER + RESERVED <= LIMIT, 'context helper exceeds reservation')
    out = bytearray(image)
    # One BL replaces the existing state() call; the wrapper tail-calls it for
    # non-conflicting IDs and for an actual extension page.
    call = struct.pack('<I', 0x94000000 | (((HELPER - HOOK) // 4) & 0x3FFFFFF))
    out[HOOK - BASE:HOOK - BASE + 4] = call
    out[HELPER - BASE:HELPER - BASE + len(HELPER_BYTES)] = HELPER_BYTES
    allowed = [(HOOK - BASE, 4), (HELPER - BASE, len(HELPER_BYTES))]
    actual = [i for i, (a, b) in enumerate(zip(image, out)) if a != b]
    miss.need(all(any(start <= i < start + count for start, count in allowed) for i in actual),
              'change outside the two approved code windows')
    return bytes(out), {'table_guards': tables, 'primary_payload_end': hex(end),
                        'helper': {'address': hex(HELPER), 'bytes': len(HELPER_BYTES), 'reserved': RESERVED},
                        'hook': {'address': hex(HOOK), 'before': image[HOOK - BASE:HOOK - BASE + 4].hex(),
                                 'after': call.hex()},
                        'changed_byte_count': len(actual)}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--input', type=Path, required=True)
    ap.add_argument('--expected-sha256', required=True)
    ap.add_argument('--build-manifest', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--receipt', type=Path, required=True)
    a = ap.parse_args()
    miss.need(not a.output.exists() and not a.receipt.exists(), 'output or receipt exists')
    original = a.input.read_bytes()
    build_bytes = a.build_manifest.read_bytes()
    out, facts = patch(original, a.expected_sha256, json.loads(build_bytes))
    a.output.parent.mkdir(parents=True, exist_ok=True)
    a.output.write_bytes(out)
    record = {'input': str(a.input), 'input_sha256': miss.sha(original), 'output': str(a.output),
              'output_sha256': miss.sha(out), 'build_manifest': str(a.build_manifest),
              'build_manifest_sha256': miss.sha(build_bytes), **facts, 'build_id_recalculated': False,
              'semantics': 'For overlapping IDs 3180..3224, native or absent current page uses original NFP text; group 2 extension pages [0x7000,0x9500) preserve all original dynamic text behavior including valid empty and miss. No 12-byte fallback patch.'}
    a.receipt.parent.mkdir(parents=True, exist_ok=True)
    a.receipt.write_text(json.dumps(record, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps(record, ensure_ascii=False))


if __name__ == '__main__':
    main()
