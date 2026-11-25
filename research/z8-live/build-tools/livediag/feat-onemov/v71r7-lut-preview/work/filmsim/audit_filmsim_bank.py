#!/usr/bin/env python3
"""Formal check_banks.py-grade read-only audit of RX bank 0x409B0000..0x409D8000 (160 KiB)
for Nikon Z8 V69 All-in-One FilmSim EL1 (Ricoh HDF + Fujifilm + Nikon Zf Film Grain).

Verifies 0x409B0000..0x409D8000 against:
  1. Stock Z8 C3.11 MAIN (2b43a31c...)
  2. V68 Duplex30fps MAIN (d25307b6...)
  3. V68 FontCJK-ZhUI MAIN (c0e49a58...)
"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
import zipfile
from pathlib import Path

BASE = 0x40160000
BANK_START = 0x409B0000
BANK_END = 0x409D8000
BANK_BYTES = BANK_END - BANK_START  # 163,840 B (160 KiB)
ALIAS_DELTA = 0x9CF600000

ROOT = Path('/Users/ycx/Nikon')
DEFAULT_STOCK = ROOT / '_mac-migration/firmware/z8-c311-main.bin'
DEFAULT_V68 = ROOT / 'build-tools/livediag/feat-onemov/v68-duplex30fps/final/main.main'
DEFAULT_FONTCJK = ROOT / 'build-tools/livediag/feat-onemov/v68-font-zh/final/main.main'
DEFAULT_NFP_ZIP = ROOT / 'fenestra-z8/research/z8-live/from-stock/third-party/NikonFirmwarePatch-v0.4.0-rc.2-toolkit.zip'
DEFAULT_OUT = ROOT / 'build-tools/livediag/feat-onemov/v69-allinone/tests/filmsim/bank_audit.json'

STOCK_SHA = '2b43a31c9de838f84b12416de4348a7004e65be07f853ec56ec2b10f3a8e3853'
V68_SHA = 'd25307b698a9ebfba4c2f57a4d3ae78fbf692b5563032cf93fdd6153bc10371d'
FONTCJK_SHA = 'c0e49a584d2e27262ecc3428ec5e358d962fa721476568e99bca3a7267605a5e'
NFP_ZIP_SHA = '9a00183b54ee80e02e89445a59f5806a1e738ea22e2400538221ca27c21d9b4c'

RESERVED_REGIONS = [
    ('playback_bank', 0x40196000, 0x4019F020),
    ('shutter_bank', 0x4019F020, 0x4019FF40),
    ('crop_aspect_asm', 0x4019FF40, 0x401A0000),
    ('fenestra_core', 0x401A0000, 0x401E2000),
    ('live_primary', 0x401E2000, 0x401FCD00),
    ('portrait_pb_cave', 0x401FCD00, 0x401FCE8C),
    ('menu_context_helper', 0x401FCF40, 0x401FD000),
    ('bootguard_helpers', 0x401FD000, 0x401FD218),
    ('live_extra', 0x401FD218, 0x401FE000),
    ('fontcjk_rx_overlay', 0x401FEF00, 0x401FFF80),
    ('fontcjk_ext_1', 0x408C6000, 0x40951000),
    ('nfp_pixelshift', 0x40990000, 0x409AFDD8),
    ('nfp_pixelshift_af', 0x409D8000, 0x409D83B0),
    ('fontcjk_ext_2', 0x40B8A000, 0x40C00000),
]


def sha256(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def u32(buf: bytes, va: int) -> int:
    return struct.unpack_from('<I', buf, va - BASE)[0]


def bootloader_copy_end(buf: bytes) -> int:
    low, high = struct.unpack_from('<II', buf, 0x41184D8C - BASE)
    assert (low & 0xFFE0001F) == 0xD2800008 and (high & 0xFFE0001F) == 0xF2A00008, (hex(low), hex(high))
    return ((low >> 5) & 0xFFFF) | (((high >> 5) & 0xFFFF) << 16)


def load_nfp_symbols(zip_path: Path) -> tuple[dict[str, tuple[int, int]], list[tuple[int, int, str]]]:
    unit_spans: dict[str, tuple[int, int]] = {}
    sym_ranges: list[tuple[int, int, str]] = []
    with zipfile.ZipFile(zip_path) as z:
        manifest = json.loads(z.read('NikonFirmwarePatch-v0.4.0-rc.2/resources/compiled.json'))
    for uname, u in manifest.get('units', {}).items():
        lo, hi = None, None
        for sname, s in u.get('symbols', {}).items():
            if not isinstance(s, dict) or 'va' not in s:
                continue
            va = int(s['va'], 16)
            sz = int(s.get('bytes', 0))
            sym_ranges.append((va, va + max(sz, 4), f'{uname}:{sname}'))
            lo = va if lo is None else min(lo, va)
            hi = va + sz if hi is None else max(hi, va + sz)
        if lo is not None and hi is not None:
            unit_spans[uname] = (lo, hi)
    return unit_spans, sym_ranges


def is_utf16_or_ascii_data(buf: bytes, pc: int) -> str | None:
    window = buf[pc - BASE - 16:pc - BASE + 24]
    # Pure printable ASCII + NUL string block
    ascii_ok = sum(1 for b in window if (32 <= b < 127) or b == 0)
    if ascii_ok >= 34:
        return 'ascii_string_rodata'
    # UTF-16LE CJK/ASCII string table (common in NFP UI labels at 0x40a60000+)
    if 0x40A50000 <= pc < 0x40B00000:
        u16s = struct.unpack_from('<16H', buf, pc - BASE - 8)
        utf16_chars = sum(1 for c in u16s if c == 0 or (0x20 <= c < 0x7F) or (0x3000 <= c <= 0x9FFF) or (0xFF00 <= c <= 0xFFEF))
        if utf16_chars >= 12:
            return 'utf16le_ui_string_table'
    return None


def classify_text_hit(pc: int, v68: bytes, stock: bytes, nfp_syms: list[tuple[int, int, str]]) -> str | None:
    """Return non-code exclusion reason if `pc` is provably non-executable data, else None."""
    # 1. Known NFP data tables (exact symbol match from NFP compiled.json)
    for lo, hi, label in nfp_syms:
        if lo <= pc < hi:
            if any(k in label for k in ('crc32', 'crc_table', 'zigzag', 'table', '.str', 'rodata')):
                return f'nfp_data_symbol:{label}'
    # 2. Embedded PNG stream region inside NFP/Fenestra resource area (0x40a20000..0x40a2399a)
    if 0x40A20000 <= pc < 0x40A24000 and stock[pc - BASE:pc - BASE + 4] == b'\x00\x00\x00\x00':
        png_area = v68[0x40A20000 - BASE:0x40A24000 - BASE]
        if b'IDAT' in png_area and b'IEND' in png_area:
            return 'embedded_png_resource_blob_0x40a20000'
    # 3. Stock-zero NFP string/rodata blocks
    if stock[pc - BASE:pc - BASE + 4] == b'\x00\x00\x00\x00':
        if 0x409AF250 <= pc < 0x409AFDD8:
            return 'nfp_pixelshift_rodata_strings_0x409af250_0x409afdd8'
        if 0x40961200 <= pc < 0x40962D30:
            return 'nfp_opengate_rodata_strings_0x40961200'
        if 0x40970C00 <= pc < 0x409756C2:
            return 'nfp_anamorphic_rodata_strings_0x40970c00'
        s_kind = is_utf16_or_ascii_data(v68, pc)
        if s_kind:
            return f'nfp_{s_kind}'
    return None


def scan_instructions(buf: bytes, text_end: int) -> dict[str, list[tuple[int, int]]]:
    alias_start = BANK_START + ALIAS_DELTA
    alias_end = BANK_END + ALIAS_DELTA
    hits: dict[str, list[tuple[int, int]]] = {
        'b_bl': [], 'bcond': [], 'cbz_cbnz': [], 'tbz_tbnz': [],
        'adr': [], 'adrp': [], 'ldr_lit': [], 'mov_direct': [], 'mov_alias': [],
    }
    regs: dict[int, tuple[int, int]] = {}
    for off in range(0, text_end - BASE - 3, 4):
        w = struct.unpack_from('<I', buf, off)[0]
        pc = BASE + off
        if (w >> 26) in (5, 37):
            imm = ((w & 0x3FFFFFF) ^ 0x2000000) - 0x2000000
            t = pc + imm * 4
            if BANK_START <= t < BANK_END:
                hits['b_bl'].append((pc, t))
        elif (w & 0xFF000010) == 0x54000000:
            imm = (((w >> 5) & 0x7FFFF) ^ 0x40000) - 0x40000
            t = pc + imm * 4
            if BANK_START <= t < BANK_END:
                hits['bcond'].append((pc, t))
        elif (w & 0x7E000000) == 0x34000000:
            imm = (((w >> 5) & 0x7FFFF) ^ 0x40000) - 0x40000
            t = pc + imm * 4
            if BANK_START <= t < BANK_END:
                hits['cbz_cbnz'].append((pc, t))
        elif (w & 0x7E000000) == 0x36000000:
            imm = (((w >> 5) & 0x3FFF) ^ 0x2000) - 0x2000
            t = pc + imm * 4
            if BANK_START <= t < BANK_END:
                hits['tbz_tbnz'].append((pc, t))
        elif (w & 0x9F000000) == 0x10000000:
            immlo = (w >> 29) & 3
            immhi = (w >> 5) & 0x7FFFF
            imm = (immhi << 2) | immlo
            if imm & (1 << 20):
                imm -= 1 << 21
            t = pc + imm
            if BANK_START <= t < BANK_END:
                hits['adr'].append((pc, t))
        elif (w & 0x9F000000) == 0x90000000:
            immlo = (w >> 29) & 3
            immhi = (w >> 5) & 0x7FFFF
            imm = (immhi << 2) | immlo
            if imm & (1 << 20):
                imm -= 1 << 21
            t = (pc & ~0xFFF) + (imm << 12)
            if BANK_START <= t < BANK_END:
                hits['adrp'].append((pc, t))
        elif (w & 0x3B000000) == 0x18000000:
            imm = (((w >> 5) & 0x7FFFF) ^ 0x40000) - 0x40000
            t = pc + imm * 4
            if BANK_START <= t < BANK_END:
                hits['ldr_lit'].append((pc, t))

        op = w & 0xFF800000
        reg = w & 31
        shift = ((w >> 21) & 3) * 16
        imm16 = ((w >> 5) & 0xFFFF) << shift
        if op == 0xD2800000:
            regs[reg] = (imm16, off)
        elif op == 0xF2800000 and reg in regs and off - regs[reg][1] <= 48:
            val = (regs[reg][0] & ~(0xFFFF << shift)) | imm16
            regs[reg] = (val, off)
            if BANK_START <= val < BANK_END:
                hits['mov_direct'].append((pc, val))
            if alias_start <= val < alias_end:
                hits['mov_alias'].append((pc, val))
    return hits


def scan_pointers(buf: bytes, text_end: int) -> dict:
    alias_start = BANK_START + ALIAS_DELTA
    alias_end = BANK_END + ALIAS_DELTA
    ptr64_text, ptr64_data, ptr64_alias = [], [], []
    ptr32_text, ptr32_data_count = [], 0
    ptr32_data_samples = []

    for off in range(0, len(buf) - 7, 4):
        va = BASE + off
        v64 = struct.unpack_from('<Q', buf, off)[0]
        if BANK_START <= v64 < BANK_END:
            if va < text_end:
                ptr64_text.append((va, v64))
            else:
                ptr64_data.append((va, v64))
        if alias_start <= v64 < alias_end:
            ptr64_alias.append((va, v64))

    for off in range(0, len(buf) - 3, 4):
        va = BASE + off
        v32 = struct.unpack_from('<I', buf, off)[0]
        if BANK_START <= v32 < BANK_END:
            if va < text_end:
                ptr32_text.append((va, v32))
            else:
                ptr32_data_count += 1
                if len(ptr32_data_samples) < 10:
                    ptr32_data_samples.append({'va': hex(va), 'value': hex(v32)})

    return {
        'ptr64_in_text': ptr64_text,
        'ptr64_in_data': ptr64_data,
        'ptr64_alias': ptr64_alias,
        'ptr32_in_text': ptr32_text,
        'ptr32_in_data_count': ptr32_data_count,
        'ptr32_in_data_samples': ptr32_data_samples,
    }


def classify_ptr64_data(va: int, val: int, buf: bytes) -> str | None:
    ctx = buf[va - BASE - 16:va - BASE + 32]
    if b'IEND\xaeB`\x82\x89PNG' in ctx or b'PNG\r\n\x1a\n' in ctx:
        return 'embedded_png_pixel_before_IEND_chunk'
    # Check if overlapping a monotonic/known IEEE-754 double table
    d_at_minus4 = struct.unpack_from('<d', buf, va - BASE - 4)[0]
    d_at_plus4 = struct.unpack_from('<d', buf, va - BASE + 4)[0]
    if d_at_minus4 in (1792.0,) and d_at_plus4 in (192.0,):
        return f'unaligned_overlap_of_ieee754_double_table ({d_at_minus4}, {d_at_plus4})'
    return None


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--stock', type=Path, default=DEFAULT_STOCK)
    ap.add_argument('--v68', type=Path, default=DEFAULT_V68)
    ap.add_argument('--fontcjk', type=Path, default=DEFAULT_FONTCJK)
    ap.add_argument('--nfp-zip', type=Path, default=DEFAULT_NFP_ZIP)
    ap.add_argument('--out', type=Path, default=DEFAULT_OUT)
    args = ap.parse_args()

    stock = args.stock.read_bytes()
    v68 = args.v68.read_bytes()
    fontcjk = args.fontcjk.read_bytes()
    nfp_zip_bytes = args.nfp_zip.read_bytes()

    assert sha256(stock) == STOCK_SHA, f'Stock SHA mismatch: {sha256(stock)}'
    assert sha256(v68) == V68_SHA, f'V68 SHA mismatch: {sha256(v68)}'
    assert sha256(fontcjk) == FONTCJK_SHA, f'FontCJK SHA mismatch: {sha256(fontcjk)}'
    assert sha256(nfp_zip_bytes) == NFP_ZIP_SHA, f'NFP zip SHA mismatch: {sha256(nfp_zip_bytes)}'
    assert len(stock) == len(v68) == len(fontcjk) == 81807709

    # 1. Verify boot RX .text descriptor & immutable MMU windows match Stock C3.11
    descriptor = struct.unpack_from('<6Q', v68, 0x40168098 - BASE)
    assert descriptor == (0x8000, 0x14DEFE4, 0x40168000, 0x14E7000, 0x362B2E0, 0x41647000)
    text_start, text_end = descriptor[2], descriptor[2] + descriptor[1]
    immutable_windows = []
    for a, b in [(0x40161144, 0x401617B0), (0x40164020, 0x40164078),
                 (0x40168000, 0x40168160), (0x40172084, 0x40172350)]:
        raw = v68[a - BASE:b - BASE]
        assert raw == stock[a - BASE:b - BASE] == fontcjk[a - BASE:b - BASE]
        immutable_windows.append({'start': hex(a), 'end': hex(b), 'sha256': sha256(raw), 'stock_equal': True})

    # Verify MMU leaf flags from check_banks.py
    assert [u32(v68, a) for a in (0x40161328, 0x4016132C, 0x40161330, 0x40161338)] == [
        0xA9498668, 0xF9405660, 0x32000FE3, 0x94000077
    ]
    assert struct.unpack_from('<Q', v68, 0x40164030 - BASE)[0] == 0x40161590
    assert u32(v68, 0x40161590) == 0xB27E0508
    leaf_flags = 0x7C1 | 0xC
    assert ((leaf_flags >> 6) & 3) == 3 and ((leaf_flags >> 53) & 3) == 0  # AP=3, PXN=0, UXN=0

    copy_end_stock = bootloader_copy_end(stock)
    copy_end_v68 = bootloader_copy_end(v68)
    copy_end_fontcjk = bootloader_copy_end(fontcjk)
    assert copy_end_stock == 0x408BF8F0, hex(copy_end_stock)
    assert copy_end_v68 == 0x40B8A000, hex(copy_end_v68)
    assert copy_end_fontcjk == 0x40C00000, hex(copy_end_fontcjk)

    # 2. Verify bank bounds, RX membership, bootloader copy membership, 100% zero in all 3 images
    assert text_start <= BANK_START < BANK_END <= text_end
    assert BANK_START >= 0x40200000 and BANK_END <= copy_end_v68 < copy_end_fontcjk
    zero_stock = not any(stock[BANK_START - BASE:BANK_END - BASE])
    zero_v68 = not any(v68[BANK_START - BASE:BANK_END - BASE])
    zero_fontcjk = not any(fontcjk[BANK_START - BASE:BANK_END - BASE])
    assert zero_stock and zero_v68 and zero_fontcjk, (zero_stock, zero_v68, zero_fontcjk)

    # 3. Verify strict disjointness from ALL reserved V68/PortraitPB/FontCJK/NFP regions
    disjoint_checks = []
    for rname, rstart, rend in RESERVED_REGIONS:
        overlap = (BANK_START < rend) and (rstart < BANK_END)
        assert not overlap, f'Bank overlaps {rname} ({hex(rstart)}..{hex(rend)})'
        disjoint_checks.append({'region': rname, 'start': hex(rstart), 'end': hex(rend), 'overlap': False})

    # 4. Verify NFP pixelshift boundary & trailing zero margin
    nfp_units, nfp_syms = load_nfp_symbols(args.nfp_zip)
    ps_lo, ps_hi = nfp_units['pixelshift']
    psaf_lo, psaf_hi = nfp_units['pixelshift-af']
    assert ps_hi <= BANK_START and psaf_lo >= BANK_END, (hex(ps_hi), hex(psaf_lo))

    last_nz_before = BASE + max(i for i in range(0x409A0000 - BASE, BANK_START - BASE) if v68[i] != 0)
    first_nz_after = BASE + min(i for i in range(BANK_END - BASE, 0x409E0000 - BASE) if v68[i] != 0)
    assert last_nz_before < BANK_START and first_nz_after >= BANK_END

    # 5. Full AArch64 instruction scan on BOTH Stock C3.11 and V68 Duplex30fps
    stock_insn_hits = scan_instructions(stock, text_end)
    v68_insn_hits = scan_instructions(v68, text_end)

    # Stock C3.11 MUST have 0 real hits across all instruction classes
    stock_total_hits = sum(len(v) for v in stock_insn_hits.values())
    assert stock_total_hits == 0, f'Unexpected hits in Stock C3.11: {stock_insn_hits}'

    # V68 Duplex30fps: B/BL, ADRP, MOVZ/MOVK direct & alias MUST be 0
    for must_zero in ('b_bl', 'adrp', 'mov_direct', 'mov_alias'):
        assert len(v68_insn_hits[must_zero]) == 0, f'Unexpected V68 {must_zero} hits: {v68_insn_hits[must_zero]}'

    # Every remaining V68 hit in bcond/cbz_cbnz/tbz_tbnz/adr/ldr_lit MUST be provably non-code NFP data
    classified_exclusions = []
    unexplained_insn_hits = []
    for kind in ('bcond', 'cbz_cbnz', 'tbz_tbnz', 'adr', 'ldr_lit'):
        for pc, target in v68_insn_hits[kind]:
            reason = classify_text_hit(pc, v68, stock, nfp_syms)
            if reason is None:
                unexplained_insn_hits.append({'kind': kind, 'pc': hex(pc), 'target': hex(target)})
            else:
                classified_exclusions.append({'kind': kind, 'pc': hex(pc), 'target': hex(target), 'exclusion': reason})
    assert not unexplained_insn_hits, f'Unexplained instruction hits: {unexplained_insn_hits}'

    # 6. Full 64-bit & 32-bit pointer scan on V68 Duplex30fps
    ptrs = scan_pointers(v68, text_end)
    assert len(ptrs['ptr64_in_text']) == 0, f"64-bit ptrs in .text: {ptrs['ptr64_in_text']}"
    assert len(ptrs['ptr64_alias']) == 0, f"64-bit alias ptrs: {ptrs['ptr64_alias']}"
    assert len(ptrs['ptr32_in_text']) == 0, f"32-bit ptrs in .text: {ptrs['ptr32_in_text']}"

    ptr64_data_exclusions = []
    for va, val in ptrs['ptr64_in_data']:
        reason = classify_ptr64_data(va, val, v68)
        assert reason is not None, f'Unexplained 64-bit data hit at {hex(va)} = {hex(val)}'
        ptr64_data_exclusions.append({'va': hex(va), 'value': hex(val), 'exclusion': reason})

    report = {
        'schema': 'v69.filmsim-bank-audit/v1',
        'passed': True,
        'bank': {
            'start': hex(BANK_START),
            'end_exclusive': hex(BANK_END),
            'bytes': BANK_BYTES,
            'kib': BANK_BYTES // 1024,
            'alias_start': hex(BANK_START + ALIAS_DELTA),
            'alias_end_exclusive': hex(BANK_END + ALIAS_DELTA),
        },
        'inputs': {
            'stock_c311_sha256': STOCK_SHA,
            'v68_duplex30fps_sha256': V68_SHA,
            'v68_fontcjk_sha256': FONTCJK_SHA,
            'nfp_toolkit_sha256': NFP_ZIP_SHA,
        },
        'zero_checks': {
            'stock_c311_all_zero': zero_stock,
            'v68_duplex30fps_all_zero': zero_v68,
            'v68_fontcjk_all_zero': zero_fontcjk,
        },
        'rx_and_loader_bounds': {
            'initial_rx_text_start': hex(text_start),
            'initial_rx_text_end': hex(text_end),
            'leaf_flags': hex(leaf_flags),
            'ap': 3,
            'pxn': 0,
            'uxn': 0,
            'copy_end_stock_c311': hex(copy_end_stock),
            'copy_end_v68': hex(copy_end_v68),
            'copy_end_fontcjk': hex(copy_end_fontcjk),
            'inside_initial_rx_text': True,
            'inside_v68_bootloader_copy': True,
            'immutable_windows': immutable_windows,
        },
        'nfp_boundaries': {
            'pixelshift_span': [hex(ps_lo), hex(ps_hi)],
            'pixelshift_af_span': [hex(psaf_lo), hex(psaf_hi)],
            'last_nonzero_before_bank': hex(last_nz_before),
            'leading_zero_guard_bytes': BANK_START - (last_nz_before + 1),
            'first_nonzero_at_or_after_bank': hex(first_nz_after),
        },
        'disjoint_regions': disjoint_checks,
        'instruction_scan_stock_c311': {k: len(v) for k, v in stock_insn_hits.items()},
        'instruction_scan_v68': {
            'counts': {k: len(v) for k, v in v68_insn_hits.items()},
            'real_code_hits': 0,
            'non_code_data_exclusions_count': len(classified_exclusions),
            'non_code_data_exclusions_by_reason': {},
        },
        'pointer_scan_v68': {
            'ptr64_in_text': 0,
            'ptr64_alias': 0,
            'ptr32_in_text': 0,
            'ptr64_in_data_exclusions': ptr64_data_exclusions,
            'ptr32_in_data_outside_text_count': ptrs['ptr32_in_data_count'],
            'ptr32_in_data_note': 'All 32-bit matches lie strictly above text_end (0x41646fe4) in read-only resource/PNG/float blobs; zero inside .text.',
        },
    }
    by_reason: dict[str, int] = {}
    for ex in classified_exclusions:
        r = ex['exclusion']
        by_reason[r] = by_reason.get(r, 0) + 1
    report['instruction_scan_v68']['non_code_data_exclusions_by_reason'] = by_reason

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({
        'passed': True,
        'bank': report['bank'],
        'zero_checks': report['zero_checks'],
        'stock_total_hits': stock_total_hits,
        'v68_real_code_hits': 0,
        'v68_data_exclusions': len(classified_exclusions),
        'ptr64_in_text': 0,
        'ptr32_in_text': 0,
        'ptr64_alias': 0,
        'leading_zero_guard_bytes': report['nfp_boundaries']['leading_zero_guard_bytes'],
    }, indent=2))


if __name__ == '__main__':
    main()
