#!/usr/bin/env python3
"""Independent exact-byte, region-isolation, and branch-target audit for V63-PortraitPB."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parent
V63 = ROOT.parent / 'v63-live1280'
BASE = 0x40160000
BUILD_ID_ADDR = 0x401FE060
EXPECTED_BASELINE_SHA256 = '1126f043ed7df1c0acd69368cb5328ac9c1896f04e4caef5b873fe568bb94866'

sys.path.insert(0, str(ROOT.parents[1]))
from safe_llvm_disasm import Disassembler  # noqa: E402
sys.path.insert(0, str(ROOT))
import patch_portrait_pb as p  # noqa: E402


def sha(data):
    return hashlib.sha256(data).hexdigest()


def cur(image, address, count):
    off = address - BASE
    return image[off:off + count]


def decode_branch(image, address):
    w = struct.unpack('<I', cur(image, address, 4))[0]
    op = w >> 26
    assert op in (5, 37), f'{address:#x}: not B/BL (word={w:#x})'
    imm = ((w & 0x3FFFFFF) ^ 0x2000000) - 0x2000000
    return ('BL' if op == 37 else 'B'), address + imm * 4


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--baseline', type=Path, default=V63 / 'final/main.main')
    ap.add_argument('--expected-baseline-sha256', default=EXPECTED_BASELINE_SHA256)
    ap.add_argument('--build-manifest', type=Path, default=V63 / 'build/build.json')
    ap.add_argument('--candidate', type=Path, default=ROOT / 'final/main.main')
    ap.add_argument('--meta', type=Path, default=ROOT / 'final/main.json')
    ap.add_argument('--output', type=Path, default=ROOT / 'final/binary-audit.json')
    a = ap.parse_args()

    old = a.baseline.read_bytes()
    new = a.candidate.read_bytes()
    meta = json.loads(a.meta.read_text())
    build = json.loads(a.build_manifest.read_text())

    assert sha(old) == a.expected_baseline_sha256
    assert sha(new) == meta['final_main_sha256']
    assert len(old) == len(new) == p.EXPECTED_SIZE

    cave_bytes, end_va, entries = p.build_cave(p.CAVE_START)
    assert cur(old, p.CAVE_START, p.CAVE_LIMIT - p.CAVE_START) == bytes(p.CAVE_LIMIT - p.CAVE_START)
    assert cur(new, p.CAVE_START, len(cave_bytes)) == cave_bytes
    assert cur(new, end_va, p.CAVE_LIMIT - end_va) == bytes(p.CAVE_LIMIT - end_va)

    scratch = bytearray(new)
    struct.pack_into('<I', scratch, BUILD_ID_ADDR - BASE, 0)
    recomputed_bid = struct.unpack('<I', hashlib.sha256(scratch).digest()[:4])[0] | 0x01000000
    assert struct.unpack_from('<I', new, BUILD_ID_ADDR - BASE)[0] == recomputed_bid
    assert hex(recomputed_bid) == meta['build_id']

    protected_regions = [
        ('playback', 0x40196000, 0x4019F020),
        ('shutter', 0x4019F020, 0x4019FF40),
        ('crop_aspect_assembly', 0x4019FF40, 0x401A0000),
        ('live_elf_primary', 0x401E2000, 0x401E2000 + build['live_payload_bytes']),
        ('menu_hook', 0x401CD410, 0x401CD414),
        ('menu_context_helper', 0x401FCF40, 0x401FD000),
        ('bootguard_metadata_helpers', 0x401FD000, 0x401FD218),
        ('live_extra_excluding_tkhd_hook', 0x401FD218, 0x401FE000),
        ('dcf_colorspace_funcs', 0x406BC444, 0x406BC470),
        ('active_movie_player_56c_orient', 0x40711308, 0x40713000),
        ('active_movie_player_56c_ui', 0x40660000, 0x40670000),
        ('active_movie_player_flag_write', 0x40635FD8, 0x40636100),
    ]
    region_audit = []
    for name, s, e in protected_regions:
        assert cur(old, s, e - s) == cur(new, s, e - s), f'protected region modified: {name}'
        region_audit.append({
            'name': name,
            'start': hex(s),
            'end_exclusive': hex(e),
            'sha256': sha(cur(new, s, e - s)),
            'identical': True,
        })

    branch_checks = []
    for name, va in p.HOOKS.items():
        mnemonic, target = decode_branch(new, va)
        expected_call = (name == 'dcf_mov')
        assert mnemonic == ('BL' if expected_call else 'B')
        assert target == entries[name]
        branch_checks.append({
            'hook': name,
            'address': hex(va),
            'instruction': mnemonic,
            'target': hex(target),
            'old_hex': cur(old, va, 4).hex(),
            'new_hex': cur(new, va, 4).hex(),
        })

    dis = Disassembler()
    cave_disasm = []
    for va in range(p.CAVE_START, end_va, 4):
        raw = cur(new, va, 4)
        width, text = dis.instruction(raw, va)
        assert width == 4 and 'udf' not in text.lower() and 'unknown' not in text.lower(), (hex(va), text)
        cave_disasm.append({'va': hex(va), 'hex': raw.hex(), 'insn': text})

    approved = set(range(p.CAVE_START - BASE, end_va - BASE))
    for va in p.HOOKS.values():
        approved.update(range(va - BASE, va - BASE + 4))
    approved.update(range(BUILD_ID_ADDR - BASE, BUILD_ID_ADDR - BASE + 4))

    changed = [i for i, (a, b) in enumerate(zip(old, new)) if a != b]
    unknown = [i for i in changed if i not in approved]
    assert not unknown, [hex(BASE + i) for i in unknown[:50]]

    result = {
        'passed': True,
        'baseline_main_sha256': sha(old),
        'final_main_sha256': sha(new),
        'build_id': hex(recomputed_bid),
        'bytes': len(new),
        'different_bytes': len(changed),
        'unexplained_difference_addresses': [hex(BASE + i) for i in unknown],
        'cave': {
            'start': hex(p.CAVE_START),
            'end_exclusive': hex(end_va),
            'payload_bytes': len(cave_bytes),
            'trailing_zero_bytes': p.CAVE_LIMIT - end_va,
            'instructions': len(cave_disasm),
        },
        'branch_checks': branch_checks,
        'protected_regions_unchanged': region_audit,
        'cave_disassembly': cave_disasm,
    }
    a.output.parent.mkdir(parents=True, exist_ok=True)
    a.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps({
        'passed': True,
        'different_bytes': len(changed),
        'unexplained_bytes': len(unknown),
        'cave_bytes': len(cave_bytes),
        'cave_instructions': len(cave_disasm),
        'trailing_zero_bytes': p.CAVE_LIMIT - end_va,
        'protected_regions_verified': len(region_audit),
        'branches_verified': len(branch_checks),
        'build_id': hex(recomputed_bid),
    }, indent=2))


if __name__ == '__main__':
    main()
