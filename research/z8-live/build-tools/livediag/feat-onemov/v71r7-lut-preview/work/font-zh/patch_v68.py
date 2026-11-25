#!/usr/bin/env python3
"""Apply the V68/V69 CJK Font Engine + Chinese UI Localization layer onto a verified Z8 MAIN.

Supports dynamic ORIG_TEXT_VA resolution from the existing `B <live_ui_menu_text>`
instruction at 0x4032e130 so it patches cleanly onto V68 Duplex30fps, V69 All-in-One
(+ HangFix + PortraitPB + FilmSim), or any future Live rebuild without hardcoded offsets.

Exact patched sites (every pre-patch byte asserted!):
  1. 0x40880d74 (set_primary_font):      b v68_set_primary_font
  2. 0x401cd328 (fx_z8_font_enter):      cbz x0, +40 -> b +36 (0x401cd34c; disables Slot-3 Font[16])
  3. 0x401fe20c (slots.state portrait):  b.ne 0x401a7898 -> b.ne v68_lang_svc (adds op 15 SLOTS_LANG_GET)
  4. 0x4032e130 (menu_text):             b <ORIG_TEXT_VA> -> b v68_menu_text (dynamic ORIG_TEXT_VA!)
  5. 0x41184d8c..0x41184d94 (loader):    copy-end 0x40b8a000 -> 0x40c00000 (within mapped 0x40200000..0x40c00000)
  6. 0x401fe060 (bootguard Build ID):    recomputed SHA-256[0:4] | 0x01000000
"""
import argparse
import hashlib
import json
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

BASE = 0x40160000
HERE = Path(__file__).resolve().parent

CODE_CAVE = 0x401FEF00
CODE_CAVE_MAX = 0x401FFF80 - CODE_CAVE  # 4,224 B

LIVE_PRIMARY_LO = 0x401E2000
LIVE_PRIMARY_HI = 0x401FCD00  # strictly before PortraitPB cave

CJK1_VA = 0x408C6000
CJK1_MAX = 0x40951000 - CJK1_VA  # 569,344 B

CJK2_VA = 0x40B8A000
CJK2_MAX = 0x40C00000 - CJK2_VA  # 483,328 B

BUILD_ID_VA = 0x401FE060
SVC_RET_VA = 0x401A7898
MENU_TEXT_HOOK_VA = 0x4032E130

EXPECTED_SITES = {
    'SET_PRIMARY_FONT': (0x40880D74, 'e80300aa'),  # mov x8, x0
    'FX_FONT_ENTER':    (0x401CD328, '400100b4'),  # cbz x0, #40
    'SLOTS_SVC_NE':     (0x401FE20C, '61b4d454'),  # b.ne 0x401a7898
    'LOADER_END_0':     (0x41184D8C, '080094d2'),  # mov x8, #0xa000
    'LOADER_END_1':     (0x41184D90, '0817a8f2'),  # movk x8, #0x40b8, lsl #16
}


def decode_b(at: int, word: int) -> int:
    assert (word >> 26) == 5, f"Expected AArch64 unconditional B at {hex(at)}, got 0x{word:08x}"
    imm = word & 0x3FFFFFF
    if imm & 0x2000000:
        imm -= 0x4000000
    return at + imm * 4


def enc_b(at: int, target: int) -> bytes:
    off = (target - at) >> 2
    assert -(1 << 25) <= off < (1 << 25), f"B out of range: {hex(at)} -> {hex(target)}"
    return struct.pack('<I', 0x14000000 | (off & 0x03FFFFFF))


def enc_bcond(at: int, target: int, cond: int) -> bytes:
    off = (target - at) >> 2
    assert -(1 << 18) <= off < (1 << 18), f"B.cond out of range: {hex(at)} -> {hex(target)}"
    return struct.pack('<I', 0x54000000 | ((off & 0x7FFFF) << 5) | (cond & 0xF))


def resolve_manifest_menu_text(manifest_path: Path):
    """Extract live_ui_menu_text address from live-ranges.json, binary-audit.json, or build.json if present."""
    data = json.loads(manifest_path.read_text(encoding='utf-8'))
    if isinstance(data.get('symbols'), dict) and 'live_ui_menu_text' in data['symbols']:
        v = data['symbols']['live_ui_menu_text']
        return int(v, 16) if isinstance(v, str) else int(v)
    if isinstance(data.get('branches_relocated'), list):
        for b in data['branches_relocated']:
            if b.get('symbol') == 'live_ui_menu_text' or str(b.get('address', '')).lower() == hex(MENU_TEXT_HOOK_VA):
                v = b['new_target']
                return int(v, 16) if isinstance(v, str) else int(v)
    # Check sibling live-ranges.json if build.json was passed
    sib = manifest_path.parent / 'live-ranges.json'
    if manifest_path.name == 'build.json' and sib.exists():
        sdata = json.loads(sib.read_text(encoding='utf-8'))
        if isinstance(sdata.get('symbols'), dict) and 'live_ui_menu_text' in sdata['symbols']:
            v = sdata['symbols']['live_ui_menu_text']
            return int(v, 16) if isinstance(v, str) else int(v)
    return None


def resolve_manifest_symbol(manifest_path: Path, sym_name: str):
    data = json.loads(manifest_path.read_text(encoding='utf-8'))
    if isinstance(data.get('symbols'), dict) and sym_name in data['symbols']:
        v = data['symbols'][sym_name]
        return int(v, 16) if isinstance(v, str) else int(v)
    return None


def compile_overlay(c_src: Path, cjk1_size: int, cjk2_size: int, orig_text_va: int, filmsim_slots_svc_va: int):
    with tempfile.TemporaryDirectory() as td:
        t = Path(td)
        obj = t / 'v68.o'
        elf = t / 'v68.elf'
        binf = t / 'v68.bin'
        lds = t / 'v68.ld'
        lds.write_text(f"""
SECTIONS {{
    . = {CODE_CAVE:#x};
    .text : {{ *(.text*) }}
    .rodata : ALIGN(8) {{ *(.rodata*) }}
    /DISCARD/ : {{ *(.comment) *(.note*) *(.eh_frame*) }}
}}
""")
        subprocess.run([
            'clang', '--target=aarch64-none-elf', '-std=c11', '-O2',
            '-ffreestanding', '-fno-builtin', '-fno-pic', '-fno-pie',
            '-mgeneral-regs-only', '-mstrict-align',
            '-fno-stack-protector', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables',
            '-Wall', '-Wextra', '-Werror',
            f'-DCJK1_SIZE={cjk1_size}u',
            f'-DCJK2_SIZE={cjk2_size}u',
            f'-DORIG_TEXT_VA=UINT64_C({orig_text_va:#x})',
            '-c', str(c_src), '-o', str(obj),
        ], check=True)
        subprocess.run([
            'ld.lld', '-T', str(lds),
            f'--defsym=svc_ret_sym={SVC_RET_VA:#x}',
            f'--defsym=filmsim_slots_svc_sym={filmsim_slots_svc_va:#x}',
            '-o', str(elf), str(obj),
        ], check=True)

        # Ensure zero writable .data / .bss / .got / .rela sections exist!
        shdr = subprocess.run(['llvm-readelf', '-S', str(elf)], capture_output=True, text=True, check=True).stdout
        for line in shdr.splitlines():
            for bad in (' .data ', ' .bss ', ' .got ', ' .rela.'):
                assert bad not in line, f"Unexpected section {bad.strip()} in overlay ELF:\n{shdr}"

        nm = subprocess.run(['llvm-nm', str(elf)], capture_output=True, text=True, check=True).stdout
        syms = {}
        for line in nm.splitlines():
            parts = line.split()
            if len(parts) == 3:
                syms[parts[2]] = int(parts[0], 16)

        subprocess.run(['llvm-objcopy', '-O', 'binary', str(elf), str(binf)], check=True)
        raw = binf.read_bytes()
        assert len(raw) <= CODE_CAVE_MAX, f"Overlay too big: {len(raw)} > {CODE_CAVE_MAX}"
        return raw, syms


def verify_chinese_glyph_coverage(c_src: Path, stock_main: bytes, cjk1: bytes, cjk2: bytes):
    """Verify every char in TR(...) entries of v68_patch_code.c is covered by Font[2]/Font[0]+Font[18]+CJK1+CJK2."""
    sys.path.insert(0, str(HERE))
    from build_cjk_hif import extract_stock_font, parse_stock_hif_cmap_and_glyphs

    cmap2, _ = parse_stock_hif_cmap_and_glyphs(extract_stock_font(stock_main, 2))
    cmap0, _ = parse_stock_hif_cmap_and_glyphs(extract_stock_font(stock_main, 0))
    cmap18, _ = parse_stock_hif_cmap_and_glyphs(extract_stock_font(stock_main, 18))
    cmap_e1, _ = parse_stock_hif_cmap_and_glyphs(cjk1)
    cmap_e2, _ = parse_stock_hif_cmap_and_glyphs(cjk2)

    cov_sc = set(cmap2) | set(cmap18) | set(cmap_e1) | set(cmap_e2)
    cov_tc = set(cmap0) | set(cmap18) | set(cmap_e1) | set(cmap_e2)

    src_text = c_src.read_text(encoding='utf-8')
    missing_sc, missing_tc = set(), set()
    checked = 0
    for line in src_text.splitlines():
        if 'TR(' not in line:
            continue
        strs = re.findall(r'"([^"]*)"', line)
        if len(strs) >= 3:
            checked += 1
            _, sc_s, tc_s = strs[0], strs[1], strs[2]
            for ch in sc_s:
                if ord(ch) not in cov_sc:
                    missing_sc.add(ch)
            for ch in tc_s:
                if ord(ch) not in cov_tc:
                    missing_tc.add(ch)
    assert checked > 0, "No TR(...) entries found in C source"
    assert not missing_sc, f"Missing SC glyphs in {c_src.name}: {''.join(sorted(missing_sc))}"
    assert not missing_tc, f"Missing TC glyphs in {c_src.name}: {''.join(sorted(missing_tc))}"
    return checked


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--base-main', type=Path, required=True)
    ap.add_argument('--expected-base-sha256', type=str, default=None,
                    help='Optional strict SHA-256 assertion for --base-main')
    ap.add_argument('--build-manifest', type=Path, default=None,
                    help='Optional live-ranges.json / binary-audit.json / build.json to cross-check live_ui_menu_text')
    ap.add_argument('--cjk1', type=Path, default=HERE / 'fonts' / 'cjk_ext_1.hif')
    ap.add_argument('--cjk2', type=Path, default=HERE / 'fonts' / 'cjk_ext_2.hif')
    ap.add_argument('--c-src', type=Path, default=HERE / 'v68_patch_code.c')
    ap.add_argument('--version', type=str, default='v69-font-zh')
    ap.add_argument('--out-main', type=Path, required=True)
    ap.add_argument('--out-json', type=Path, required=True)
    args = ap.parse_args()

    orig = args.base_main.read_bytes()
    assert len(orig) == 81807709, f"Unexpected MAIN size: {len(orig)}"
    base_sha = hashlib.sha256(orig).hexdigest()
    if args.expected_base_sha256:
        assert base_sha == args.expected_base_sha256.lower(), (
            f"Base MAIN SHA-256 mismatch: got {base_sha}, expected {args.expected_base_sha256}"
        )

    cjk1 = args.cjk1.read_bytes()
    cjk2 = args.cjk2.read_bytes()
    assert 0 < len(cjk1) <= CJK1_MAX and cjk1[:4] == b'hif\x01'
    assert 0 < len(cjk2) <= CJK2_MAX and cjk2[:4] == b'hif\x01'

    tr_count = verify_chinese_glyph_coverage(args.c_src, orig, cjk1, cjk2)
    print(f"Glyph coverage check for {args.c_src.name}: {tr_count} TR() rows, 100% covered (SC + TC)!")

    # Verify bootguard v2 signature
    old_bid = struct.unpack_from('<I', orig, BUILD_ID_VA - BASE)[0]
    assert (old_bid & 0x01000000) and orig[0x401FE068 - BASE:0x401FE070 - BASE] == struct.pack('<Q', 0x31544F4F425846), (
        "Base MAIN does not carry a valid Bootguard v2 signature at 0x401fe060..0x401fe070"
    )

    # Verify static pre-patch site bytes
    for name, (addr, hx) in EXPECTED_SITES.items():
        got = orig[addr - BASE:addr - BASE + 4].hex()
        assert got == hx, f"Site {name} @ {hex(addr)}: expected {hx}, got {got}"

    # Verify SVC_RET_VA epilogue instruction
    svc_ret_got = orig[SVC_RET_VA - BASE:SVC_RET_VA - BASE + 4].hex()
    assert svc_ret_got == 'f44f49a9', f"Unexpected SVC_RET_VA @ {hex(SVC_RET_VA)}: {svc_ret_got}"

    # Dynamically resolve ORIG_TEXT_VA from existing B <live_ui_menu_text> at 0x4032e130!
    menu_word = struct.unpack_from('<I', orig, MENU_TEXT_HOOK_VA - BASE)[0]
    orig_text_va = decode_b(MENU_TEXT_HOOK_VA, menu_word)
    assert LIVE_PRIMARY_LO <= orig_text_va < LIVE_PRIMARY_HI, (
        f"Decoded 0x4032e130 target {hex(orig_text_va)} outside live_primary "
        f"[{hex(LIVE_PRIMARY_LO)}, {hex(LIVE_PRIMARY_HI)})"
    )
    manifest_target = None
    if args.build_manifest is not None:
        manifest_target = resolve_manifest_menu_text(args.build_manifest)
        if manifest_target is not None:
            assert orig_text_va == manifest_target, (
                f"0x4032e130 target {hex(orig_text_va)} disagrees with manifest live_ui_menu_text {hex(manifest_target)}"
            )
    print(f"Resolved dynamic ORIG_TEXT_VA from 0x4032e130 (word=0x{menu_word:08x}): {hex(orig_text_va)}"
          + (f" (manifest-verified)" if manifest_target is not None else ""))

    # Resolve filmsim_slots_svc_trampoline from Stage 1 manifest (or fallback to SVC_RET_VA if absent)
    filmsim_slots_svc_va = SVC_RET_VA
    if args.build_manifest is not None:
        fs_va = resolve_manifest_symbol(args.build_manifest, 'filmsim_slots_svc_trampoline')
        if fs_va is not None:
            assert 0x409B0000 <= fs_va < 0x409D8000, f"filmsim_slots_svc_trampoline {hex(fs_va)} outside .filmsim!"
            filmsim_slots_svc_va = fs_va
            print(f"Resolved dynamic filmsim_slots_svc_trampoline: {hex(filmsim_slots_svc_va)}")

    # Verify all 3 caves are 100% zero
    assert not any(orig[CODE_CAVE - BASE:CODE_CAVE - BASE + CODE_CAVE_MAX]), "CODE_CAVE (0x401fef00..0x401fff80) not zero!"
    assert not any(orig[CJK1_VA - BASE:CJK1_VA - BASE + CJK1_MAX]), "CJK1 cave (0x408c6000..0x40951000) not zero!"
    assert not any(orig[CJK2_VA - BASE:CJK2_VA - BASE + CJK2_MAX]), "CJK2 cave (0x40b8a000..0x40c00000) not zero!"

    overlay, syms = compile_overlay(args.c_src, len(cjk1), len(cjk2), orig_text_va, filmsim_slots_svc_va)
    print(f"Compiled overlay: {len(overlay)} / {CODE_CAVE_MAX} B (free={CODE_CAVE_MAX - len(overlay)} B)")
    for k in ('v68_set_primary_font', 'v68_lang_svc', 'v68_menu_text'):
        assert k in syms, f"Missing symbol {k}"
        print(f"  {k} = 0x{syms[k]:08x}")

    img = bytearray(orig)

    # 1. Embed CJK_EXT_1 & CJK_EXT_2
    img[CJK1_VA - BASE:CJK1_VA - BASE + len(cjk1)] = cjk1
    img[CJK2_VA - BASE:CJK2_VA - BASE + len(cjk2)] = cjk2

    # 2. Embed compiled overlay code & const tables
    img[CODE_CAVE - BASE:CODE_CAVE - BASE + len(overlay)] = overlay

    # 3. Patch hook sites
    patches = {
        0x40880D74: enc_b(0x40880D74, syms['v68_set_primary_font']),
        0x401CD328: enc_b(0x401CD328, 0x401CD34C),  # unconditionally set x0=0, x2=0 -> lock-only, no Slot-3 Font[16]
        0x401FE20C: enc_bcond(0x401FE20C, syms['v68_lang_svc'], cond=1),  # b.ne v68_lang_svc
        MENU_TEXT_HOOK_VA: enc_b(MENU_TEXT_HOOK_VA, syms['v68_menu_text']),
        # Loader copy end: 0x40b8a000 -> 0x40c00000
        0x41184D8C: struct.pack('<I', 0xD2800008),  # mov x8, #0
        0x41184D90: struct.pack('<I', 0xF2A81808),  # movk x8, #0x40c0, lsl #16
    }
    for addr, raw in patches.items():
        img[addr - BASE:addr - BASE + len(raw)] = raw

    # 4. Restamp Bootguard Build ID at 0x401fe060
    struct.pack_into('<I', img, BUILD_ID_VA - BASE, 0)
    new_bid = struct.unpack('<I', hashlib.sha256(bytes(img)).digest()[:4])[0] | 0x01000000
    struct.pack_into('<I', img, BUILD_ID_VA - BASE, new_bid)

    out_bytes = bytes(img)
    out_sha = hashlib.sha256(out_bytes).hexdigest()
    args.out_main.parent.mkdir(parents=True, exist_ok=True)
    args.out_main.write_bytes(out_bytes)
    args.out_json.parent.mkdir(parents=True, exist_ok=True)

    info = {
        'version': args.version,
        'base_main_sha256': base_sha,
        'main_sha256': out_sha,
        'build_id': hex(new_bid),
        'previous_build_id': hex(old_bid),
        'orig_text_va': hex(orig_text_va),
        'filmsim_slots_svc_va': hex(filmsim_slots_svc_va),
        'orig_menu_text_word': f'0x{menu_word:08x}',
        'manifest_menu_text_verified': manifest_target is not None,
        'translations_count': tr_count,
        'overlay': {
            'va': hex(CODE_CAVE),
            'bytes': len(overlay),
            'max_bytes': CODE_CAVE_MAX,
            'free_bytes': CODE_CAVE_MAX - len(overlay),
            'symbols': {k: hex(v) for k, v in sorted(syms.items()) if k.startswith('v68_')},
        },
        'cjk_ext_1': {'va': hex(CJK1_VA), 'bytes': len(cjk1), 'sha256': hashlib.sha256(cjk1).hexdigest()},
        'cjk_ext_2': {'va': hex(CJK2_VA), 'bytes': len(cjk2), 'sha256': hashlib.sha256(cjk2).hexdigest()},
        'patches': {hex(a): b.hex() for a, b in sorted(patches.items())},
        'physical_camera_verified': False,
    }
    args.out_json.write_text(json.dumps(info, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(info, indent=2))


if __name__ == '__main__':
    main()
