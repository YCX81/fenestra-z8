#!/usr/bin/env python3
"""Independent exact-byte, branch-target, dynamic ORIG_TEXT_VA, bootguard, and font-blob audit
for the V68/V69 FontCJK-ZhUI stage."""
import argparse
import hashlib
import json
import struct
from pathlib import Path

BASE = 0x40160000
HERE = Path(__file__).resolve().parent
LIVE_PRIMARY_LO = 0x401E2000
LIVE_PRIMARY_HI = 0x401FCD00


def sha(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def decode_b(at: int, word: int) -> int:
    assert (word >> 26) == 5, f"Not B at {hex(at)}: {hex(word)}"
    imm = word & 0x3FFFFFF
    if imm & 0x2000000:
        imm -= 0x4000000
    return at + imm * 4


def decode_bcond(at: int, word: int, expected_cond: int) -> int:
    assert (word >> 24) == 0x54 and (word & 0xF) == expected_cond, (
        f"Not B.cond({expected_cond}) at {hex(at)}: {hex(word)}"
    )
    imm = (word >> 5) & 0x7FFFF
    if imm & 0x40000:
        imm -= 0x80000
    return at + imm * 4


def verify_menu_text_calls_target(buf: bytes, fn_va: int, expected_target: int) -> bool:
    """Verify compiled v68_menu_text prologue materializes expected_target via MOVZ/MOVK (or B/BL)."""
    lo16 = expected_target & 0xFFFF
    hi16 = (expected_target >> 16) & 0xFFFF
    movz_regs = {}
    for off in range(0, 0x80, 4):
        va = fn_va + off
        w = struct.unpack_from('<I', buf, va - BASE)[0]
        # Direct B/BL check
        if (w >> 26) in (0x05, 0x25):
            imm = w & 0x3FFFFFF
            if imm & 0x2000000:
                imm -= 0x4000000
            if va + imm * 4 == expected_target:
                return True
        # MOVZ wRd/xRd, #imm16 (hw=0)
        if (w & 0x7FE00000) == 0x52800000:
            rd = w & 0x1F
            imm16 = (w >> 5) & 0xFFFF
            if imm16 == lo16:
                movz_regs[rd] = True
        # MOVK wRd/xRd, #imm16, lsl #16 (hw=1)
        if (w & 0x7FE00000) == 0x72A00000:
            rd = w & 0x1F
            imm16 = (w >> 5) & 0xFFFF
            if imm16 == hi16 and movz_regs.get(rd):
                return True
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--base-main', type=Path, required=True)
    ap.add_argument('--expected-base-sha256', type=str, default=None)
    ap.add_argument('--v68-main', type=Path, required=True)
    ap.add_argument('--v68-json', type=Path, required=True)
    ap.add_argument('--cjk1', type=Path, default=HERE / 'fonts' / 'cjk_ext_1.hif')
    ap.add_argument('--cjk2', type=Path, default=HERE / 'fonts' / 'cjk_ext_2.hif')
    ap.add_argument('--out-json', type=Path, required=True)
    args = ap.parse_args()

    old = args.base_main.read_bytes()
    new = args.v68_main.read_bytes()
    meta = json.loads(args.v68_json.read_text(encoding='utf-8'))
    cjk1 = args.cjk1.read_bytes()
    cjk2 = args.cjk2.read_bytes()

    assert len(old) == len(new) == 81807709
    old_sha = sha(old)
    new_sha = sha(new)
    if args.expected_base_sha256:
        assert old_sha == args.expected_base_sha256.lower(), (
            f"Base SHA mismatch: got {old_sha}, expected {args.expected_base_sha256}"
        )
    expected_meta_base = meta.get('base_main_sha256') or meta.get('base_v67_ultra30fps_sha256')
    assert old_sha == expected_meta_base, f"Base SHA mismatch vs meta JSON: {old_sha} vs {expected_meta_base}"
    assert new_sha == meta['main_sha256'], f"Candidate SHA mismatch vs meta JSON: {new_sha} vs {meta['main_sha256']}"

    # Verify Bootguard Build ID at 0x401fe060
    bid_off = 0x401FE060 - BASE
    scratch = bytearray(new)
    struct.pack_into('<I', scratch, bid_off, 0)
    expected_bid = struct.unpack('<I', hashlib.sha256(bytes(scratch)).digest()[:4])[0] | 0x01000000
    actual_bid = struct.unpack_from('<I', new, bid_off)[0]
    assert actual_bid == expected_bid and hex(actual_bid) == meta['build_id']

    syms = {k: int(v, 16) for k, v in meta['overlay']['symbols'].items()}
    ov_va = int(meta['overlay']['va'], 16)
    ov_len = meta['overlay']['bytes']
    c1_va = int(meta['cjk_ext_1']['va'], 16)
    c2_va = int(meta['cjk_ext_2']['va'], 16)

    assert ov_va == 0x401FEF00 and ov_va + ov_len <= 0x401FFF80

    # Verify all 3 caves were 100% zero in base
    assert not any(old[ov_va - BASE:ov_va - BASE + ov_len]), "Overlay cave not zero in base"
    assert not any(old[c1_va - BASE:c1_va - BASE + len(cjk1)]), "CJK1 cave not zero in base"
    assert not any(old[c2_va - BASE:c2_va - BASE + len(cjk2)]), "CJK2 cave not zero in base"

    # Verify embedded CJK_EXT_1 and CJK_EXT_2 match byte-for-byte
    assert new[c1_va - BASE:c1_va - BASE + len(cjk1)] == cjk1
    assert new[c2_va - BASE:c2_va - BASE + len(cjk2)] == cjk2

    def u32(buf, va):
        return struct.unpack_from('<I', buf, va - BASE)[0]

    # Dynamically verify ORIG_TEXT_VA resolved from base 0x4032e130
    orig_text_va = decode_b(0x4032E130, u32(old, 0x4032E130))
    assert LIVE_PRIMARY_LO <= orig_text_va < LIVE_PRIMARY_HI, f"orig_text_va {hex(orig_text_va)} out of bounds"
    assert hex(orig_text_va) == meta['orig_text_va'], (
        f"orig_text_va mismatch: decoded {hex(orig_text_va)}, meta {meta['orig_text_va']}"
    )
    assert verify_menu_text_calls_target(new, syms['v68_menu_text'], orig_text_va), (
        f"v68_menu_text ({hex(syms['v68_menu_text'])}) does not materialize dynamic ORIG_TEXT_VA {hex(orig_text_va)}!"
    )

    # Verify all branch targets & loader instructions
    assert decode_b(0x40880D74, u32(new, 0x40880D74)) == syms['v68_set_primary_font']
    assert decode_b(0x401CD328, u32(new, 0x401CD328)) == 0x401CD34C
    assert decode_bcond(0x401FE20C, u32(new, 0x401FE20C), 1) == syms['v68_lang_svc']
    assert decode_b(0x4032E130, u32(new, 0x4032E130)) == syms['v68_menu_text']
    assert u32(new, 0x41184D8C) == 0xD2800008  # mov x8, #0
    assert u32(new, 0x41184D90) == 0xF2A81808  # movk x8, #0x40c0, lsl #16

    # Verify ALL V68/V69/V71.7 critical banks (including PortraitPB, FilmSim, Bank 1 LUT Core & Bank 2 LUT Menu!) are 100% BYTE-IDENTICAL!
    preserved = [
        ('playback_bank',       0x40196000, 0x4019F020),
        ('shutter_bank',        0x4019F020, 0x401A0000),
        ('live_primary_bank',   0x401E2000, 0x401FCD00),
        ('portrait_pb_cave',    0x401FCD00, 0x401FCE8C),
        ('menu_context_helper', 0x401FCF40, 0x401FD000),
        ('live_extra_bank',     0x401FD218, 0x401FE000),
        ('cardslot_bank',       0x401FE300, 0x401FEF00),
        ('v71r7_lut_menu_bank', 0x40981000, 0x40990000),
        ('filmsim_bank',        0x409B0000, 0x409D8000),
        ('v71r7_lut_core_bank', 0x409DB000, 0x409EC000),
        ('nfp_strings_bank',    0x40A50000, 0x40AF0000),
    ]
    for pname, ps, pe in preserved:
        assert old[ps - BASE:pe - BASE] == new[ps - BASE:pe - BASE], f"Preserved bank {pname} modified!"

    # Build exact approved diff byte set
    approved_spans = [
        ('fx_z8_font_enter', 0x401CD328, 4),
        ('bootguard_build_id', 0x401FE060, 4),
        ('slots_state_lang_branch', 0x401FE20C, 4),
        ('v68_rx_overlay', ov_va, ov_len),
        ('menu_text_hook', 0x4032E130, 4),
        ('set_primary_font_hook', 0x40880D74, 4),
        ('cjk_ext_1_hif', c1_va, len(cjk1)),
        ('cjk_ext_2_hif', c2_va, len(cjk2)),
        ('loader_copy_end_0x40c00000', 0x41184D8C, 8),
    ]
    approved = set()
    for _, va, sz in approved_spans:
        approved.update(range(va - BASE, va - BASE + sz))

    # Fast byte-diff check across entire 81.8 MB image!
    changed_count = 0
    unknown = []
    for off in range(0, len(old), 65536):
        end = min(off + 65536, len(old))
        if old[off:end] != new[off:end]:
            for idx in range(off, end):
                if old[idx] != new[idx]:
                    changed_count += 1
                    if idx not in approved:
                        unknown.append(BASE + idx)

    assert not unknown, f"Unexplained byte diffs: {[hex(a) for a in unknown[:30]]}"

    report = {
        'passed': True,
        'version': meta.get('version', 'v69-font-zh'),
        'baseline_main_sha256': old_sha,
        'final_main_sha256': new_sha,
        'build_id': hex(actual_bid),
        'orig_text_va': hex(orig_text_va),
        'orig_text_call_verified': True,
        'bytes': len(new),
        'different_bytes': changed_count,
        'unexplained_bytes': 0,
        'preserved_banks_identical': [name for name, _, _ in preserved],
        'approved_regions': [dict(name=n, va=hex(va), bytes=sz) for n, va, sz in approved_spans],
        'branch_targets_verified': {
            '0x40880d74': hex(syms['v68_set_primary_font']),
            '0x401cd328': '0x401cd34c',
            '0x401fe20c': hex(syms['v68_lang_svc']),
            '0x4032e130': hex(syms['v68_menu_text']),
        },
        'loader_copy_end': '0x40c00000',
    }
    args.out_json.parent.mkdir(parents=True, exist_ok=True)
    args.out_json.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
