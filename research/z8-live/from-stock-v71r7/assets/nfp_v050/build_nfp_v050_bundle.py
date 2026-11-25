#!/usr/bin/env python3
"""Reproducible local builder for V71.5 NFP v0.5.1 relocated upgrade bundle.

Reads strictly read-only upstream XinmiaoZhao/NikonFirmwarePatch (v0.4.0, v0.5.0,
and v0.5.1 @ 25fefa1) via `git archive` / `git show` into a temporary directory
and stock Nikon Z8 C3.11 MAIN (2b43a31c...), compiles all 20 ARM64 units with
local Apple clang 21.0.0 (including the 10 relocated/modified units + new v0.5.1
`framing-fixes` unit), composes both un-relocated and relocated full 4-group NFP
v0.5.1 (and relocated v0.5.0 for exact v0.5.0 -> v0.5.1 delta verification), runs
exhaustive binary / pointer / Capstone disassembly / upstream contract audits,
and emits:
  - build/nfp_v050/manifest.json
  - build/nfp_v050/v050_bundle.bin  (zlib-compressed payload blob)

Preserves all 4 V71.x safe relocations:
  - Region A (sdr-heif + photo_shared): 0x40B70000..0x40B72000 -> 0x40AF8000..0x40AFA000 (-0x78000)
  - Region B (photo-areas Part 1):      0x40B7C000..0x40B85000 -> 0x40B12000..0x40B1B000 (-0x6A000)
  - Region C (photo-areas Part 2):      0x40B90000..0x40B97000 -> 0x40AF0000..0x40AF7000 (-0xA0000)
  - Region D (Q3_STASH RW-alias fix):   0x40950000 (0xA0FF50000) -> 0x40952000 (0xA0FF52000) (+0x2000)

And handles all 3 commutative/superset special sites cleanly outside `apply_v050`:
  - 0x407138D0 (aspect_fix_chain): preserves Stage 0 aspect_fix (b 0x4019ffc0) if installed;
    otherwise installs v0.5.1 `b 0x40968400` (evf_fit_hook) on stock/standalone builds.
  - 0x40CF4620..0x40CF4634 (pool6_slot_size): preserves Stage 0 5b superset 0x001C2000
    (1280x960x1.5) if already >= 0x0018D800 (NEVER downgrades to 0x0018D800); otherwise
    raises stock 0x00151800 -> v0.5.1 0x0018D800 (1280x848x1.5).
  - 0x41184D8C..0x41184D94 (loader_copy_end): preserves any boundary >= 0x40B50000
    (Stage 0 C64 0x40B8A000 / Stage 3 FontCJK 0x40C00000); raises stock to 0x40B50000.
"""
from __future__ import annotations

import binascii
import hashlib
import io
import json
import pathlib
import re
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
import zlib

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN

HERE = pathlib.Path(__file__).resolve().parent
UPSTREAM = pathlib.Path('/Users/ycx/Nikon/external/NikonFirmwarePatch-upstream')
STOCK_MAIN_PATH = pathlib.Path('/Users/ycx/Nikon/_mac-migration/firmware/z8-c311-main.bin')
EXPECTED_STOCK_SHA = '2b43a31c9de838f84b12416de4348a7004e65be07f853ec56ec2b10f3a8e3853'

BASE = 0x40160000
DELTA_HEIF = -0x78000
DELTA_AREA1 = -0x6A000
DELTA_AREA2 = -0xA0000

RELOC_ASM_UNITS = [
    ('heif-format',    'z8heif/format/hooks.S',    0x40B70000, 0x40B718D0, DELTA_HEIF),
    ('heif-capture',   'z8heif/capture/hooks.S',   0x40B718D0, 0x40B71950, DELTA_HEIF),
    ('heif-srgb',      'z8heif/srgb/hooks.S',      0x40B71950, 0x40B71A00, DELTA_HEIF),
    ('heif-drive',     'z8heif/drive/hooks.S',     0x40B71B00, 0x40B71D00, DELTA_HEIF),
    ('heif-icc',       'z8heif/icc/hooks.S',       0x40B71D00, 0x40B72000, DELTA_HEIF),
    ('area-formula',   'z8area/formula/hooks.S',   0x40B7D800, 0x40B7E000, DELTA_AREA1),
    ('area-sizes',     'z8area/sizes/hooks.S',     0x40B82000, 0x40B84000, DELTA_AREA1),
    ('area-thumbnail', 'z8area/thumbnail/hooks.S', 0x40B91200, 0x40B91400, DELTA_AREA2),
    ('area-quick',     'z8area/quick/hooks.S',     0x40B91800, 0x40B92000, DELTA_AREA2),
]

FORBIDDEN_BANKS = (
    ('low_v71_caves', 0x40196000, 0x40200000),
    ('cjk_ext_1',     0x408C6000, 0x40951000),
    ('q3_stash_zero', 0x40952000, 0x40953000),
    ('filmsim',       0x409B0000, 0x409D8000),
    ('c64_and_cjk2',  0x40B50000, 0x40C00000),
)

SPECIAL_OFFS_V040 = {0x407138D0 - BASE, 0x41184D8C - BASE}
SPECIAL_OFFS_V051 = {0x407138D0 - BASE, 0x40CF4620 - BASE, 0x41184D8C - BASE}


def sha(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def export_git_tag(repo: pathlib.Path, tag: str, dst: pathlib.Path) -> None:
    """Strictly read-only export of a git tag into dst."""
    tar_bytes = subprocess.check_output(['git', '-C', str(repo), 'archive', tag])
    with tarfile.open(fileobj=io.BytesIO(tar_bytes), mode='r:') as tf:
        tf.extractall(dst)


def patch_file(root: pathlib.Path, rel: str, subs: list[tuple[str, str]]) -> None:
    p = root / rel
    txt = p.read_text(encoding='utf-8')
    for old, new in subs:
        cnt = txt.count(old)
        assert cnt == 1, f'{rel}: expected 1 match for {old!r}, got {cnt}'
        txt = txt.replace(old, new, 1)
    p.write_text(txt, encoding='utf-8')


def import_fresh(root: pathlib.Path):
    """Clear upstream module caches and import fresh from root."""
    prefixes = ('features', 'z8patch', 'z8opengate', 'z8anamorphic', 'z8prores',
                'z8h265', 'z8log', 'z8quality', 'z8heif', 'z8area', 'z8pixelshift', 'z8precapture')
    for k in list(sys.modules):
        if k == 'features' or any(k == p or k.startswith(p + '.') for p in prefixes):
            del sys.modules[k]
    sys.path.insert(0, str(root))


def compile_and_relocate_tree(src_dir: pathlib.Path, reloc_dir: pathlib.Path, tag_label: str) -> dict:
    """Compile all upstream units bit-exact, then compile & bit-audit the 10 relocated/modified units
    and patch reloc_dir Python/JSON sources for Regions A/B/C/D."""
    import_fresh(src_dir)
    from z8patch import compiled as up_compiled
    from z8patch.asm import link_elf as asm_link_elf
    from z8opengate.linker import link_elf as og_link_elf

    cjson_up = json.loads((src_dir / 'resources/compiled.json').read_text(encoding='utf-8'))['units']
    compiler_ver = subprocess.check_output(['clang', '--version'], text=True).splitlines()[0]

    # 1. Compile ALL upstream units verbatim and assert 100% bit-exact match with compiled.json
    orig_bins: dict[str, bytes] = {}
    for uname in sorted(cjson_up):
        code, meta = up_compiled.compile_unit(uname, 'clang')
        exp = cjson_up[uname]
        assert len(code) == exp['bytes'], f'{tag_label} {uname}: len {len(code)} != {exp["bytes"]}'
        assert sha(code) == exp['sha256'], f'{tag_label} {uname}: sha mismatch'
        assert meta['symbols'] == exp['symbols'], f'{tag_label} {uname}: symbols mismatch'
        orig_bins[uname] = code
        (src_dir / 'resources' / f'{uname}.bin').write_bytes(code)
    print(f'[{tag_label}] Compiled & bit-verified all {len(orig_bins)} upstream units against compiled.json ({compiler_ver}).')

    # 2. Clone src_dir -> reloc_dir and build relocated unit binaries
    if reloc_dir.exists():
        shutil.rmtree(reloc_dir)
    shutil.copytree(src_dir, reloc_dir)

    units_manifest: dict[str, dict] = {}
    reloc_names = {x[0] for x in RELOC_ASM_UNITS} | {'opengate'}
    for uname in sorted(cjson_up):
        if uname not in reloc_names:
            raw = orig_bins[uname]
            (reloc_dir / 'resources' / f'{uname}.bin').write_bytes(raw)
            units_manifest[uname] = dict(
                relocated=False,
                bytes=len(raw),
                sha256=sha(raw),
                symbols=cjson_up[uname]['symbols'],
            )

    # 2a. Compile opengate with -DQ3_STASH_RAM=0xa0ff52000 (Region D)
    with tempfile.TemporaryDirectory() as td:
        obj = pathlib.Path(td) / 'og.o'
        flags = [
            '-target', 'aarch64-none-elf', '-x', 'assembler-with-cpp',
            '-DFULL_OUTPUT=1', '-DWITH_Q3=1', '-DQ3_STASH_RAM=0xa0ff52000',
            '-DQ3_FINE_TABLE=0x40953000', '-DQ3_BASIC_TABLE=0x409533f0', '-DOG_TIER_DELTA=0x4000',
        ]
        subprocess.run(['clang', *flags, '-c', str(src_dir / 'z8opengate/src/d10_hooks.S'), '-o', str(obj)], check=True)
        raw, entries, _relocs = og_link_elf(obj.read_bytes())
        code = bytearray(raw)
        off = 0x40960CBC - 0x40960000
        assert code[off:off + 4] == bytes.fromhex('1f1100b9')
        target = entries['og_leave_display_hook']
        struct.pack_into('<I', code, off, 0x94000000 | (((target - 0x40960CBC) // 4) & 0x3FFFFFF))
        code = bytes(code)

        orig_og = orig_bins['opengate']
        assert len(code) == len(orig_og), (len(code), len(orig_og))
        syms = {k: dict(va=hex(v), bytes=0) for k, v in entries.items()}
        assert syms == cjson_up['opengate']['symbols'], 'opengate symbols shifted!'

        wdiffs = []
        for i in range(0, len(code), 4):
            w0 = struct.unpack_from('<I', orig_og, i)[0]
            w1 = struct.unpack_from('<I', code, i)[0]
            if w0 != w1:
                wdiffs.append((hex(0x40960000 + i), hex(w0), hex(w1)))
        assert len(wdiffs) == 1, f'Expected exactly 1 opengate word diff, got {wdiffs}'
        va_s, w0_s, w1_s = wdiffs[0]
        assert int(va_s, 16) == 0x409628F0 and int(w0_s, 16) == 0x0FF50000 and int(w1_s, 16) == 0x0FF52000, wdiffs
        (reloc_dir / 'resources/opengate.bin').write_bytes(code)
        units_manifest['opengate'] = dict(
            relocated=True,
            change='Q3_STASH_RAM: 0xa0ff50000 -> 0xa0ff52000',
            bytes=len(code),
            sha256=sha(code),
            symbols=syms,
            word_diffs=wdiffs,
        )

    # 2b. Compile the 9 relocated asm units (Regions A/B/C) & bit-audit every word diff
    for name, rel_src, orig_base, orig_limit, delta in RELOC_ASM_UNITS:
        new_base = orig_base + delta
        new_limit = orig_limit + delta
        src_text = (src_dir / rel_src).read_text(encoding='utf-8')
        if name == 'heif-drive':
            def repl(m):
                old_va = int(m.group(1), 16)
                assert 0x40B70000 <= old_va < 0x40B718D0, hex(old_va)
                return f'original_{old_va + DELTA_HEIF:08X}'
            src_text, nsub = re.subn(r'original_(40B7[0-9A-Fa-f]{4})', repl, src_text)
            assert nsub == 4, nsub
        elif name == 'area-quick':
            assert 'movk x1,#0x40b9,lsl #16' in src_text
            src_text = src_text.replace('movk x1,#0x40b9,lsl #16', 'movk x1,#0x40af,lsl #16', 1)
            assert '0x40b9' not in src_text.lower()

        with tempfile.TemporaryDirectory() as td:
            sfile = pathlib.Path(td) / 'hooks.S'
            sfile.write_text(src_text, encoding='utf-8')
            obj = pathlib.Path(td) / 'patch.o'
            subprocess.run(['clang', '-target', 'aarch64-none-elf', '-x', 'assembler-with-cpp',
                            '-c', str(sfile), '-o', str(obj)], check=True)
            code, entries, _relocs = asm_link_elf(obj.read_bytes(), new_base)

        assert new_base + len(code) <= new_limit, f'{name} overflow'
        orig_bin = orig_bins[name]
        assert len(code) == len(orig_bin), f'{name} size changed'
        orig_syms = cjson_up[name]['symbols']
        new_syms = {k: dict(va=hex(v), bytes=0) for k, v in entries.items()}
        assert set(new_syms) == set(orig_syms)
        for k in orig_syms:
            assert int(new_syms[k]['va'], 16) - int(orig_syms[k]['va'], 16) == delta, (name, k)

        bad = []
        for i in range(0, len(code), 4):
            w0 = struct.unpack_from('<I', orig_bin, i)[0]
            w1 = struct.unpack_from('<I', code, i)[0]
            if w0 == w1:
                continue
            if (w0 >> 26) in (0x05, 0x25) and (w1 >> 26) == (w0 >> 26):
                def btgt(base_a, off_a, w):
                    imm = w & 0x3FFFFFF
                    if imm & 0x2000000:
                        imm -= 0x4000000
                    return base_a + off_a + 4 * imm
                t0 = btgt(orig_base, i, w0)
                t1 = btgt(new_base, i, w1)
                if t0 == t1 or (name == 'heif-drive' and t1 - t0 == DELTA_HEIF):
                    continue
                bad.append((hex(new_base + i), hex(t0), hex(t1)))
            elif name == 'area-quick' and (w0 & 0xFFE0001F) == 0xF2A00001 and (w1 & 0xFFE0001F) == 0xF2A00001:
                assert ((w0 >> 5) & 0xFFFF) == 0x40B9 and ((w1 >> 5) & 0xFFFF) == 0x40AF
            else:
                bad.append((hex(new_base + i), hex(w0), hex(w1)))
        assert not bad, f'{name} unexpected word diffs: {bad}'
        (reloc_dir / 'resources' / f'{name}.bin').write_bytes(code)
        units_manifest[name] = dict(
            relocated=True,
            orig_base=hex(orig_base),
            new_base=hex(new_base),
            new_limit=hex(new_limit),
            delta=hex(delta),
            bytes=len(code),
            sha256=sha(code),
            symbols=new_syms,
        )

    # 3. Apply surgical, asserted relocations to Python / JSON / .S files in reloc_dir
    patch_file(reloc_dir, 'z8opengate/quality.py', [
        ('CODE_AT, DATA_AT, GUI_AT, TEXT_AT, TABLES_AT, STASH_AT = [x - BASE for x in (1083510784, 1083518976, 1083523072, 1083531264, 1083531264, 1083506688)]',
         'CODE_AT, DATA_AT, GUI_AT, TEXT_AT, TABLES_AT, STASH_AT = [x - BASE for x in (1083510784, 1083518976, 1083523072, 1083531264, 1083531264, 1083514880)]'),
    ])
    patch_file(reloc_dir, 'z8opengate/patcher.py', [
        ('Q3_STASH, Q3_CODE, Q3_DATA, Q3_GUI, Q3_GUI_END, OG_TIERS = (1083506688, 1083510784, 1083518976, 1083523072, 1083531264, 1083535360)',
         'Q3_STASH, Q3_CODE, Q3_DATA, Q3_GUI, Q3_GUI_END, OG_TIERS = (1083514880, 1083510784, 1083518976, 1083523072, 1083531264, 1083535360)'),
    ])
    patch_file(reloc_dir, 'z8patch/photo_shared.py', [
        ('DISPATCH = 0x40B71A00', 'DISPATCH = 0x40AF9A00'),
    ])
    patch_file(reloc_dir, 'z8heif/drive_policy.py', [
        ('BASE = 0x40B70000\nAT = 0x40B71B00', 'BASE = 0x40AF8000\nAT = 0x40AF9B00'),
    ])
    patch_file(reloc_dir, 'z8heif/patcher.py', [
        ('CODE_AT, CODE_LIMIT = 0x40B70000, 0x40B718D0', 'CODE_AT, CODE_LIMIT = 0x40AF8000, 0x40AF98D0'),
        ("img.put(0x40B71D00,icc,'JPEG ICC header writer',expect_zero=True)",
         "img.put(0x40AF9D00,icc,'JPEG ICC header writer',expect_zero=True)"),
        ("require(end <= 0x40B718D0, 'HEIF menu outside its allocation')",
         "require(end <= 0x40AF98D0, 'HEIF menu outside its allocation')"),
        ("for name, start in [('heif-capture',0x40B718D0),('heif-srgb',0x40B71950)]:",
         "for name, start in [('heif-capture',0x40AF98D0),('heif-srgb',0x40AF9950)]:"),
    ])
    patch_file(reloc_dir, 'z8area/alloc.json', [
        ('"0x40B7D000",\n      "0x40B7E000"', '"0x40B13000",\n      "0x40B14000"'),
        ('"0x40B90000",\n      "0x40BA0000"', '"0x40AF0000",\n      "0x40AF7000"'),
        ('"loader_end": "0x40BA0000"', '"loader_end": "0x40B50000"'),
    ])
    patch_file(reloc_dir, 'z8area/geometry.py', [
        ('DATA_AT, DATA_END = 0x40B7C000, 0x40B7D000', 'DATA_AT, DATA_END = 0x40B12000, 0x40B13000'),
    ])
    patch_file(reloc_dir, 'z8area/live_view.py', [
        ('CAVE_A, CAVE_B = 0x40B7C400, 0x40B7C440', 'CAVE_A, CAVE_B = 0x40B12400, 0x40B12440'),
    ])
    patch_file(reloc_dir, 'z8area/playback.py', [
        ('CAVE_BOX_A, CAVE_BOX_B = 0x40B7C480, 0x40B7C4C0', 'CAVE_BOX_A, CAVE_BOX_B = 0x40B12480, 0x40B124C0'),
        ('assert not any(eg[0x40B7C464 - hb1.BASE:0x40B7C480 - hb1.BASE]) and a + 4 * len(ca) <= c and c + 4 * len(cb) <= 0x40B7D000',
         'assert not any(eg[0x40B12464 - hb1.BASE:0x40B12480 - hb1.BASE]) and a + 4 * len(ca) <= c and c + 4 * len(cb) <= 0x40B13000'),
    ])
    patch_file(reloc_dir, 'z8area/focus_icons.py', [
        ('CAVE_CORNER, CAVE_POINTS, CAVE_PNG, CAVE_END = 0x40B7C500, 0x40B7C580, 0x40B7C700, 0x40B7D000',
         'CAVE_CORNER, CAVE_POINTS, CAVE_PNG, CAVE_END = 0x40B12500, 0x40B12580, 0x40B12700, 0x40B13000'),
    ])
    patch_file(reloc_dir, 'z8area/focus_bounds.py', [
        ('CAVE_MAX = 0x40B7C800', 'CAVE_MAX = 0x40B12800'),
        ('CAVE_CORNER2 = 0x40B7C840', 'CAVE_CORNER2 = 0x40B12840'),
        ('CAVE_GEOM = 0x40B7C920', 'CAVE_GEOM = 0x40B12920'),
    ])
    patch_file(reloc_dir, 'z8area/playback_wide.py', [
        ('BOX_A_SITE, BOX_B_SITE, CAVE_BOX_A, CAVE_BOX_B = 0x40790DF4, 0x40790E3C, 0x40B7C480, 0x40B7C4C0',
         'BOX_A_SITE, BOX_B_SITE, CAVE_BOX_A, CAVE_BOX_B = 0x40790DF4, 0x40790E3C, 0x40B12480, 0x40B124C0'),
        ('def apply(img, eg, at=0x40B7E000, limit=0x40B80000):',
         'def apply(img, eg, at=0x40B14000, limit=0x40B16000):'),
    ])
    patch_file(reloc_dir, 'z8area/menu.py', [
        ('def apply(img, eg, at=0x40B80000, limit=0x40B84000, icons=None):',
         'def apply(img, eg, at=0x40B16000, limit=0x40B1A000, icons=None):'),
    ])
    patch_file(reloc_dir, 'z8area/icons_wide.py', [
        ('def apply(img, eg, at=0x40B84000, limit=0x40B90000, playback_index=None, icons_json=None):',
         'def apply(img, eg, at=0x40B1A000, limit=0x40B20000, playback_index=None, icons_json=None):'),
    ])
    patch_file(reloc_dir, 'z8area/thumbnail.py', [
        ('THUMB = 0x40B91200\nCAPACITY = 0x40B9017C', 'THUMB = 0x40AF1200\nCAPACITY = 0x40AF017C'),
    ])
    patch_file(reloc_dir, 'z8area/quick_picker.py', [
        ('AT = 0x40B91800\nROWS, EVENTS, TOGGLE = 0x40B91400, 0x40B91540, 0x40B91580',
         'AT = 0x40AF1800\nROWS, EVENTS, TOGGLE = 0x40AF1400, 0x40AF1540, 0x40AF1580'),
        ('variants, trampolines = 0x40B95000,0x40B92000',
         'variants, trampolines = 0x40AF5000,0x40AF2000'),
        ('assert trampolines+len(glue)<=0x40B92800',
         'assert trampolines+len(glue)<=0x40AF2800'),
    ])
    patch_file(reloc_dir, 'z8area/grid.py', [
        ('START = 0x40B92800\nLIMIT = 0x40B95000', 'START = 0x40AF2800\nLIMIT = 0x40AF5000'),
    ])
    patch_file(reloc_dir, 'z8area/patcher.py', [
        ("wide.apply(img,stock,blocks,layout,dict(cave_a=0x40B7C400,cave_b=0x40B7C440,",
         "wide.apply(img,stock,blocks,layout,dict(cave_a=0x40B12400,cave_b=0x40B12440,"),
        ("for site, name, old in ((0x40B84038,'tier_star_res_idx',0xD299AB09),\n                                (0x40B840AC,'tier_star_res_val',0xD299AB08)):",
         "for site, name, old in ((0x40B1A038,'tier_star_res_idx',0xD299AB09),\n                                (0x40B1A0AC,'tier_star_res_val',0xD299AB08)):"),
        ("img.put(0x40B82000,code,'localized image sizes and native playback size rules',expect_zero=True)",
         "img.put(0x40B18000,code,'localized image sizes and native playback size rules',expect_zero=True)"),
        ("(0xA029CB68C, 'I', 2, 0), (0x40B90854, 'I', 0x01560480, 0x01520488),\n"
         "        (0x40B90864, 'I', 0x002C0000, 0x002C0002), (0x40B9096A, 'H', 257, 256),\n"
         "        (0x40B90972, 'H', 286, 288), (0x40B90A14, 'I', 0x01560002, 0x01520002),\n"
         "        (0x40B90A44, 'I', 0x04800AC0, 0x04880AC0), (0x40B90A32, 'H', 286, 288),\n"
         "        (0xA02A96288, 'Q', 0x40B90A10, 0x40B91040)):",
         "(0xA029CB68C, 'I', 2, 0), (0x40AF0854, 'I', 0x01560480, 0x01520488),\n"
         "        (0x40AF0864, 'I', 0x002C0000, 0x002C0002), (0x40AF096A, 'H', 257, 256),\n"
         "        (0x40AF0972, 'H', 286, 288), (0x40AF0A14, 'I', 0x01560002, 0x01520002),\n"
         "        (0x40AF0A44, 'I', 0x04800AC0, 0x04880AC0), (0x40AF0A32, 'H', 286, 288),\n"
         "        (0xA02A96288, 'Q', 0x40AF0A10, 0x40AF1040)):"),
        ("block = bytearray(img.new[0x40B90D90-BASE:0x40B90DC8-BASE])",
         "block = bytearray(img.new[0x40AF0D90-BASE:0x40AF0DC8-BASE])"),
        ("img.put(0x40B91040,block,'C-low descriptor for every quality',expect_zero=True)",
         "img.put(0x40AF1040,block,'C-low descriptor for every quality',expect_zero=True)"),
        ("img.put(0x40B7D800,code,'C-low formula and 2.39 AF coordinate transforms',expect_zero=True)",
         "img.put(0x40B13800,code,'C-low formula and 2.39 AF coordinate transforms',expect_zero=True)"),
        ("img.put(0x410B7C64,branch(0x410B7C64,0x40B7D800),'C-low formula dispatch',expect=struct.pack('<I',0x540000AB))",
         "img.put(0x410B7C64,branch(0x410B7C64,0x40B13800),'C-low formula dispatch',expect=struct.pack('<I',0x540000AB))"),
        ("geometry=[1368,576], loader_floor=hex(0x40BA0000))",
         "geometry=[1368,576], loader_floor=hex(0x40B50000))"),
    ])
    patch_file(reloc_dir, 'z8patch/shared.py', [
        ("if 'still-photo-tools' in features:floor=max(floor,0x40BA0000)",
         "if 'still-photo-tools' in features:floor=max(floor,0x40B50000)"),
    ])
    patch_file(reloc_dir, 'z8patch/contract.py', [
        ("need(end>=0x40BA0000,'crop data and zero-state loader floor')",
         "need(end>=0x40B50000,'crop data and zero-state loader floor')"),
        ("need(read(new,'<Q',0x4DA1288)[0]==0x40B91040,'fixed C-low geometry pointer')",
         "need(read(new,'<Q',0x4DA1288)[0]==0x40AF1040,'fixed C-low geometry pointer')"),
        ("need(read(new,'<H',0x40B91040-base+10)[0]==576,'fixed crop height')",
         "need(read(new,'<H',0x40AF1040-base+10)[0]==576,'fixed crop height')"),
        ("groups = read(new,'<II',0x40B80000-base+44*2)",
         "groups = read(new,'<II',0x40B16000-base+44*2)"),
        ("need(groups[0]==3238 and read(new,'<I',0x40B80000-base+44*5)[0]==3239,'crop label references')",
         "need(groups[0]==3238 and read(new,'<I',0x40B16000-base+44*5)[0]==3239,'crop label references')"),
        ("need(read(new,'<I',0x40B80000-base+44*row+20)[0]==icon,'still menu icon')",
         "need(read(new,'<I',0x40B16000-base+44*row+20)[0]==icon,'still menu icon')"),
        ("need(read(new,'<H',0x40B84000-base+0x140)[0]==0x100,'still icon descriptors')",
         "need(read(new,'<H',0x40B1A000-base+0x140)[0]==0x100,'still icon descriptors')"),
    ])
    patch_file(reloc_dir, 'z8heif/drive/hooks.S', [
        ('original_40B70680', 'original_40AF8680'),
        ('original_40B703E8', 'original_40AF83E8'),
        ('original_40B700DC', 'original_40AF80DC'),
        ('original_40B700FC', 'original_40AF80FC'),
    ])
    patch_file(reloc_dir, 'z8area/quick/hooks.S', [
        ('movk x1,#0x40b9,lsl #16', 'movk x1,#0x40af,lsl #16'),
    ])
    patch_file(reloc_dir, 'z8patch/compiled.py', [
        ("-DQ3_STASH_RAM=0xa0ff50000", "-DQ3_STASH_RAM=0xa0ff52000"),
        ("('heif-icc','z8heif/icc',0x40B71D00,0x40B72000),", "('heif-icc','z8heif/icc',0x40AF9D00,0x40AFA000),"),
        ("('heif-format','z8heif/format',0x40B70000,0x40B718D0),", "('heif-format','z8heif/format',0x40AF8000,0x40AF98D0),"),
        ("('heif-capture','z8heif/capture',0x40B718D0,0x40B71950),", "('heif-capture','z8heif/capture',0x40AF98D0,0x40AF9950),"),
        ("('heif-drive','z8heif/drive',0x40B71B00,0x40B71D00),", "('heif-drive','z8heif/drive',0x40AF9B00,0x40AF9D00),"),
        ("('area-thumbnail','z8area/thumbnail',0x40B91200,0x40B91400),", "('area-thumbnail','z8area/thumbnail',0x40AF1200,0x40AF1400),"),
        ("('area-quick','z8area/quick',0x40B91800,0x40B92000),", "('area-quick','z8area/quick',0x40AF1800,0x40AF2000),"),
        ("('heif-srgb','z8heif/srgb',0x40B71950,0x40B71A00),", "('heif-srgb','z8heif/srgb',0x40AF9950,0x40AF9A00),"),
        ("('area-sizes','z8area/sizes',0x40B82000,0x40B84000),", "('area-sizes','z8area/sizes',0x40B18000,0x40B1A000),"),
        ("('area-formula','z8area/formula',0x40B7D800,0x40B7E000),", "('area-formula','z8area/formula',0x40B13800,0x40B14000),"),
    ])

    import_fresh(reloc_dir)
    from z8patch import compiled as reloc_compiled
    cjson_path = reloc_dir / 'resources/compiled.json'
    cjson = json.loads(cjson_path.read_text(encoding='utf-8'))
    for uname, uinfo in units_manifest.items():
        cjson['units'][uname]['bytes'] = uinfo['bytes']
        cjson['units'][uname]['sha256'] = uinfo['sha256']
        cjson['units'][uname]['symbols'] = uinfo['symbols']
        cjson['units'][uname]['source_sha256'] = reloc_compiled.source_files(uname)
    cjson_path.write_text(json.dumps(cjson, indent=2) + '\n', encoding='utf-8')

    return dict(compiler=compiler_ver, units=units_manifest)


def audit_relocated_image(stock: bytes, orig_nfp: bytes, reloc_nfp: bytes, edits: list[dict]) -> dict:
    """Exhaustive 4-way audit: forbidden banks, 32/64-bit pointer scan, Capstone ARM64 scan,
    and non-cave diff comparison vs un-relocated upstream NFP v0.5.1."""
    # 1. Forbidden regions check
    for fname, flo, fhi in FORBIDDEN_BANKS:
        hits = [e for e in edits if e['offset'] < fhi - BASE and flo - BASE < e['end']]
        nz = sum(1 for b in reloc_nfp[flo - BASE:fhi - BASE] if b)
        assert len(hits) == 0 and nz == 0, (fname, hits, nz)

    # 2. Residual 32/64-bit pointer scan for old 0x40B7..0x40BA or 0xA0FF50000
    old_ptr_hits = []
    for e in edits:
        data = e['new']
        va = BASE + e['offset']
        for off in range(0, len(data) - 7, 4):
            q = struct.unpack_from('<Q', data, off)[0]
            if (0x40B70000 <= q < 0x40BA0000) or q == 0xA0FF50000:
                old_ptr_hits.append((hex(va + off), hex(q), e['purpose'][:60]))
        for off in range(0, len(data) - 3, 4):
            w = struct.unpack_from('<I', data, off)[0]
            if (0x40B70000 <= w < 0x40BA0000) or w == 0x0FF50000:
                old_ptr_hits.append((hex(va + off), hex(w), e['purpose'][:60]))
    assert not old_ptr_hits, f'Residual old pointer hits: {old_ptr_hits[:10]}'

    # 3. Capstone ARM64 instruction/branch scan across all code edits
    md = Cs(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN)
    bad_insns = []
    for e in edits:
        va = BASE + e['offset']
        if va >= 0x42000000 or (0x40A50000 <= va < 0x40AF0000):
            continue
        data = e['new']
        if len(data) % 4 != 0:
            continue
        for ins in md.disasm(data, va):
            if ins.mnemonic in ('b', 'bl', 'b.eq', 'b.ne', 'b.hi', 'b.hs', 'b.lo', 'b.ls', 'cbz', 'cbnz', 'tbz', 'tbnz'):
                m = re.search(r'#0x([0-9a-fA-F]+)', ins.op_str)
                if m:
                    tgt = int(m.group(1), 16)
                    if 0x40B70000 <= tgt < 0x40BA0000:
                        bad_insns.append((hex(ins.address), ins.mnemonic, ins.op_str, e['purpose'][:60]))
            elif ins.mnemonic == 'movk':
                if any(x in ins.op_str.lower() for x in ('#0x40b7', '#0x40b8', '#0x40b9', '#0x40ba', '#0xff50')):
                    bad_insns.append((hex(ins.address), ins.mnemonic, ins.op_str, e['purpose'][:60]))
    assert not bad_insns, f'Residual ARM64 instruction hits: {bad_insns[:10]}'

    # 4. Non-cave word-by-word comparison against un-relocated upstream v0.5.1
    unexplained = []
    diff_outside = 0
    for off in range(0, len(stock) - 3, 4):
        va = BASE + off
        if 0x408BF8F0 <= va < 0x40C00000:
            continue
        b_orig = orig_nfp[off:off + 4]
        b_rel = reloc_nfp[off:off + 4]
        if b_orig == b_rel:
            continue
        diff_outside += 1
        wo = struct.unpack('<I', b_orig)[0]
        wr = struct.unpack('<I', b_rel)[0]
        if va in (0x41184D8C, 0x41184D90):
            continue
        if (wo >> 26) in (5, 0x25) and (wr >> 26) == (wo >> 26):
            def btgt(w):
                imm = w & 0x3FFFFFF
                if imm & 0x2000000:
                    imm -= 0x4000000
                return va + 4 * imm
            if btgt(wr) - btgt(wo) in (DELTA_HEIF, DELTA_AREA1, DELTA_AREA2):
                continue
        if (wr - wo) in (DELTA_HEIF, DELTA_AREA1, DELTA_AREA2) and (0x40B70000 <= wo < 0x40BA0000):
            continue
        if (wo & 0xFFE0001F) == (wr & 0xFFE0001F) and (wo >> 23) in (0x1A5, 0x1E5):
            imm_o = (wo >> 5) & 0xFFFF
            imm_r = (wr >> 5) & 0xFFFF
            hw = (wo >> 21) & 3
            if hw == 1 and imm_o in (0x40B7, 0x40B8, 0x40B9) and imm_r in (0x40AF, 0x40B1):
                continue
            if hw == 0 and ((imm_r - imm_o) & 0xFFFF) in (0x8000, 0x6000):
                continue
        unexplained.append((hex(va), b_orig.hex(), b_rel.hex()))
    assert not unexplained, f'Unexplained non-cave diffs vs upstream v0.5.1: {unexplained[:20]}'

    cave_ends = [BASE + e['end'] for e in edits if 0x408BF8F0 <= BASE + e['offset'] < 0x40C00000]
    max_cave_va = max(cave_ends)
    assert max_cave_va < 0x40B50000, f'Relocated cave exceeds 0x40B50000: {max_cave_va:#x}'
    return dict(
        forbidden_banks_clean=True,
        residual_old_pointers=0,
        residual_bad_insns=0,
        non_cave_reloc_word_diffs=diff_outside,
        unexplained_non_cave_diffs=0,
        max_cave_va=hex(max_cave_va),
    )


def main() -> None:
    # 1. Verify upstream git provenance & stock C3.11 MAIN (strictly read-only)
    v051_commit = subprocess.check_output(['git', '-C', str(UPSTREAM), 'rev-parse', 'v0.5.1^{commit}'], text=True).strip()
    v050_commit = subprocess.check_output(['git', '-C', str(UPSTREAM), 'rev-parse', 'v0.5.0^{commit}'], text=True).strip()
    v040_commit = subprocess.check_output(['git', '-C', str(UPSTREAM), 'rev-parse', 'v0.4.0^{commit}'], text=True).strip()
    v040_comb = json.loads(subprocess.check_output(['git', '-C', str(UPSTREAM), 'show', 'v0.4.0:resources/combinations.json'], text=True))

    stock = STOCK_MAIN_PATH.read_bytes()
    assert sha(stock) == EXPECTED_STOCK_SHA, f'Unexpected stock MAIN SHA: {sha(stock)}'
    eg = stock + struct.pack('>H', binascii.crc_hqx(stock, 0))

    with tempfile.TemporaryDirectory(prefix='nfp051_build_') as td_str:
        td = pathlib.Path(td_str)

        # 2. Export, compile, relocate, and compose v0.5.1 (un-relocated + relocated)
        v051_src = td / 'v051_up'
        v051_rel = td / 'v051_reloc'
        export_git_tag(UPSTREAM, 'v0.5.1', v051_src)
        units_man = compile_and_relocate_tree(v051_src, v051_rel, 'v0.5.1')

        # 2a. Compose un-relocated v0.5.1 & cross-check against upstream combinations.json
        import_fresh(v051_src)
        from features import FEATURES as UP_FEATURES, compose as up_compose
        from z8patch.contract import verify_contract as up_verify_contract
        img_up, _det_up, _rows_up = up_compose(eg, UP_FEATURES, code_source='prebuilt', check_recipe=False)
        orig_v051 = bytes(img_up.new[:len(stock)])
        up_contract = up_verify_contract(eg, bytes(img_up.new), UP_FEATURES)
        assert up_contract['status'] == 'PASS', up_contract

        v051_comb = json.loads((v051_src / 'resources/combinations.json').read_text(encoding='utf-8'))
        fk51 = '+'.join(UP_FEATURES)
        exp51 = [v051_comb['patches'][i] for i in v051_comb['profiles'][fk51]['patches']]
        assert len(img_up.edits) == len(exp51) == 1323
        for e, p in zip(sorted(img_up.edits, key=lambda x: x['offset']), sorted(exp51, key=lambda x: int(x['offset'], 16))):
            assert e['offset'] == int(p['offset'], 16) and (e['end'] - e['offset']) == p['bytes']
            assert sha(e['old']) == p['old_sha256'] and sha(e['new']) == p['new_sha256']
        print(f'[v0.5.1] Un-relocated compose verified 100% bit-exact ({len(exp51)}/1323 patches, contract PASS).')

        # 2b. Compose relocated v0.5.1 & verify upstream contract (relocated)
        import_fresh(v051_rel)
        from features import FEATURES, compose as rel_compose
        from z8patch.contract import verify_contract as rel_verify_contract
        img_rel51, detail_rel51, _rows_rel51 = rel_compose(eg, FEATURES, code_source='prebuilt', check_recipe=False)
        reloc_v051 = bytes(img_rel51.new[:len(stock)])
        rel51_contract = rel_verify_contract(eg, bytes(img_rel51.new), FEATURES)
        assert rel51_contract['status'] == 'PASS', rel51_contract
        audit51 = audit_relocated_image(stock, orig_v051, reloc_v051, img_rel51.edits)
        print(f'[v0.5.1] Relocated compose + 4-way audit PASS: {json.dumps(audit51)}')
        print(f'[v0.5.1] Relocated upstream contract.py PASS: {json.dumps(rel51_contract)}')

        # 3. Also compose relocated v0.5.0 to record exact v0.5.0 -> v0.5.1 delta SHAs
        #    and prove ZERO string/font table changes between v0.5.0 and v0.5.1!
        v050_src = td / 'v050_up'
        v050_rel = td / 'v050_reloc'
        export_git_tag(UPSTREAM, 'v0.5.0', v050_src)
        compile_and_relocate_tree(v050_src, v050_rel, 'v0.5.0')
        import_fresh(v050_rel)
        from features import FEATURES as F50, compose as rel50_compose
        img_rel50, _det50, _rows50 = rel50_compose(eg, F50, code_source='prebuilt', check_recipe=False)
        reloc_v050 = bytes(img_rel50.new[:len(stock)])

        # Verify 100% identical string pool (0x40A50000..0x40AE2000) & language directory tables
        assert reloc_v050[0x40A50000 - BASE:0x40AE2000 - BASE] == reloc_v051[0x40A50000 - BASE:0x40AE2000 - BASE], \
            'String pool / offset tables changed between v0.5.0 and v0.5.1!'
        assert reloc_v050[0x311C5A0:0x311C5A0 + 36 * 8] == reloc_v051[0x311C5A0:0x311C5A0 + 36 * 8], \
            'Language directory pointers changed between v0.5.0 and v0.5.1!'
        print('[v0.5.0 -> v0.5.1] Verified 0-byte diff across all 36 language tables & UTF-16 string pool (ZhUI 0 missing glyphs).')

        # 4. Extract all 451 non-special v0.4.0 revert windows
        full_key_v040 = [k for k in v040_comb['profiles'] if len(k.split('+')) == 4][0]
        v040_patches = [v040_comb['patches'][i] for i in v040_comb['profiles'][full_key_v040]['patches']]
        assert len(v040_patches) == 453, len(v040_patches)

        blob = bytearray()
        def add_blob(data: bytes) -> int:
            off = len(blob)
            blob.extend(data)
            return off

        revert_v040 = []
        v040_playback_fit_hex = None
        for p in sorted(v040_patches, key=lambda x: int(x['offset'], 16)):
            off = int(p['offset'], 16)
            sz = p['bytes']
            va = BASE + off
            st_bytes = stock[off:off + sz]
            assert sha(st_bytes) == p['old_sha256'], f'v040 stock mismatch at {va:#x}'
            if off in SPECIAL_OFFS_V040:
                if va == 0x407138D0:
                    # Both v0.4.0 and v0.5.0 branch to 0x409708F0 (ap_playback_fit_hook)
                    v040_playback_fit_hex = reloc_v050[off:off + 4].hex()
                    assert sha(bytes.fromhex(v040_playback_fit_hex)) == p['new_sha256']
                continue
            is_zero = not any(st_bytes)
            rec = dict(
                offset=off,
                va=hex(va),
                size=sz,
                v040_sha256=p['new_sha256'],
                stock_zero=is_zero,
                purpose=p['purpose'],
            )
            if not is_zero:
                rec['stock_sha256'] = p['old_sha256']
                if sz <= 32:
                    rec['stock_hex'] = st_bytes.hex()
                else:
                    rec['stock_blob_off'] = add_blob(st_bytes)
            revert_v040.append(rec)
        assert len(revert_v040) == 451, len(revert_v040)

        # 5. Extract all 1,320 non-special relocated v0.5.1 apply windows (key kept as `apply_v050`
        #    for full backwards compatibility with audit_whole_image.py & stage0 tooling)
        sorted_edits = sorted(img_rel51.edits, key=lambda e: e['offset'])
        for a, b in zip(sorted_edits, sorted_edits[1:]):
            assert a['end'] <= b['offset'], f'Overlapping edits: {a} vs {b}'

        apply_windows = []
        special_apply: dict[int, dict] = {}
        v050_delta_count = 0
        for e in sorted_edits:
            off = e['offset']
            sz = e['end'] - e['offset']
            va = BASE + off
            st_bytes = stock[off:off + sz]
            nw_bytes = reloc_v051[off:off + sz]
            v50_bytes = reloc_v050[off:off + sz]
            assert st_bytes == e['old'] and nw_bytes == e['new']
            if off in SPECIAL_OFFS_V051:
                special_apply[va] = dict(
                    offset=off,
                    va=hex(va),
                    size=sz,
                    stock_hex=st_bytes.hex(),
                    v050_hex=v50_bytes.hex(),
                    new_hex=nw_bytes.hex(),
                    purpose=e['purpose'],
                )
                continue
            is_zero = not any(st_bytes)
            rec = dict(
                offset=off,
                va=hex(va),
                size=sz,
                expect_zero=is_zero,
                new_sha256=sha(nw_bytes),
                blob_off=add_blob(nw_bytes),
                purpose=e['purpose'],
            )
            if not is_zero:
                rec['expect_sha256'] = sha(st_bytes)
                if sz <= 32:
                    rec['expect_hex'] = st_bytes.hex()
            if v50_bytes != nw_bytes:
                rec['v050_sha256'] = sha(v50_bytes)
                rec['v050_zero'] = not any(v50_bytes)
                v050_delta_count += 1
            apply_windows.append(rec)

        assert len(apply_windows) == 1320, len(apply_windows)
        assert v050_delta_count == 6, f'Expected exactly 6 non-special v0.5.0 -> v0.5.1 deltas, got {v050_delta_count}'
        assert set(special_apply.keys()) == {0x407138D0, 0x40CF4620, 0x41184D8C}

        # Verify exact bytes of the 3 special sites
        aspect_fix_hex = 'bc31ea17'  # b 0x4019ffc0 (Stage 0 aspect_fix -> tail-branch 0x409708f0)
        sp_fit = special_apply[0x407138D0]
        assert sp_fit['v050_hex'] == v040_playback_fit_hex
        # Verify v0.5.1 PLAYBACK_FIT branches to 0x40968400 (evf_fit_hook)
        fit_word = struct.unpack('<I', bytes.fromhex(sp_fit['new_hex']))[0]
        assert (fit_word >> 26) == 5
        fit_imm = ((fit_word & 0x3FFFFFF) ^ 0x2000000) - 0x2000000
        assert 0x407138D0 + fit_imm * 4 == 0x40968400

        sp_pool6 = special_apply[0x40CF4620]
        assert sp_pool6['size'] == 20
        pool6_middle = sp_pool6['stock_hex'][8:32]
        assert sp_pool6['new_hex'][8:32] == pool6_middle
        # Stage 0 5b Pool #6 superset: 0x001C2000 (mov w13, #0x2000 / movk w13, #0x1c, lsl #16)
        pool6_stage0_hex = struct.pack('<I', 0x5284000D).hex() + pool6_middle + struct.pack('<I', 0x72A0038D).hex()

        raw_blob = bytes(blob)
        z_blob = zlib.compress(raw_blob, level=9)
        assert zlib.decompress(z_blob) == raw_blob

        (HERE / 'v050_bundle.bin').write_bytes(z_blob)

        manifest = dict(
            schema='v71r5.nfp_v051_bundle/v1',
            upstream=dict(
                repo='XinmiaoZhao/NikonFirmwarePatch',
                target_tag='v0.5.1',
                target_commit=v051_commit,
                previous_tag='v0.5.0',
                previous_commit=v050_commit,
                reverted_tag='v0.4.0',
                reverted_commit=v040_commit,
                reverted_profile=full_key_v040,
                features=list(FEATURES),
                compiler=units_man['compiler'],
            ),
            framing_fixes=dict(
                unit='framing-fixes',
                code_va='0x40968000',
                code_end='0x40968830',
                cave_limit='0x4096f000',
                code_bytes=units_man['units']['framing-fixes']['bytes'],
                code_sha256=units_man['units']['framing-fixes']['sha256'],
                entries=detail_rel51['framing-fixes']['entries'],
                af_geometry_sites=detail_rel51['framing-fixes']['af_geometry_sites'],
                af_points_site=detail_rel51['framing-fixes']['af_points'],
                playback_fit_site=detail_rel51['framing-fixes']['playback_fit'],
                pool6_slot_site='0x40cf4620..0x40cf4634',
                upstream_movie_output_slot_bytes=0x18D800,
                v71_stage0_superset_slot_bytes=0x1C2000,
                v050_to_v051_non_special_deltas=v050_delta_count,
            ),
            relocations=dict(
                region_a_sdr_heif=dict(
                    orig_range=['0x40b70000', '0x40b72000'],
                    new_range=['0x40af8000', '0x40afa000'],
                    delta='-0x78000',
                    reason='Vacate C64 data bank (0x40b70000..0x40b8a000)',
                ),
                region_b_photo_areas_part1=dict(
                    orig_range=['0x40b7c000', '0x40b85000'],
                    new_range=['0x40b12000', '0x40b1b000'],
                    delta='-0x6a000',
                    reason='Vacate C64 data bank (0x40b70000..0x40b8a000)',
                ),
                region_c_photo_areas_part2=dict(
                    orig_range=['0x40b90000', '0x40b97000'],
                    new_range=['0x40af0000', '0x40af7000'],
                    delta='-0xa0000',
                    reason='Vacate Stage 3 cjk_ext_2.hif bank (0x40b8a000..0x40c00000)',
                ),
                region_d_q3_stash_ram=dict(
                    orig_va='0x40950000',
                    orig_rw_alias='0xa0ff50000',
                    new_va='0x40952000',
                    new_rw_alias='0xa0ff52000',
                    delta='+0x2000',
                    reason='Fix latent runtime write collision with Stage 3 cjk_ext_1.hif (0x408c6000..0x40950f07)',
                ),
                loader_copy_end=dict(
                    site='0x41184d8c',
                    nfp_v051_upstream='0x40ba0000',
                    nfp_v051_relocated_min='0x40b50000',
                    v71r5_stage0='0x40b8a000',
                    v71r5_stage3='0x40c00000',
                    reason='All relocated NFP v0.5.1 caves end <= 0x40b488d0 (< 0x40b50000); Stage 0 C64 boundary 0x40b8a000 preserved for Stage 3.',
                ),
                pool6_slot_size_superset=dict(
                    site='0x40cf4620..0x40cf4634',
                    stock='0x00151800',
                    nfp_v051_min='0x0018d800',
                    v71r5_stage0='0x001c2000',
                    reason='Stage 0 5b 0x001C2000 (1280x960x1.5) is a strict superset of NFP v0.5.1 0x0018D800 (1280x848x1.5) with +215,040 B headroom per slot and also covers Live Photo 1280x852 (0x18F600) and 4:3 EVF ceiling; never downgraded.',
                ),
            ),
            audit=dict(
                relocated_audit=audit51,
                upstream_relocated_contract=rel51_contract,
                v050_to_v051_string_tables_identical=True,
                unrelocated_v051_main_sha256=sha(orig_v051),
            ),
            units=units_man['units'],
            stock_c311_main_sha256=sha(stock),
            reloc_v050_on_stock_main_sha256=sha(reloc_v050),
            reloc_on_stock_main_sha256=sha(reloc_v051),
            bundle=dict(
                file='v050_bundle.bin',
                compression='zlib',
                zlib_bytes=len(z_blob),
                zlib_sha256=sha(z_blob),
                raw_bytes=len(raw_blob),
                raw_sha256=sha(raw_blob),
            ),
            special_sites=dict(
                aspect_fix_chain=dict(
                    va='0x407138d0',
                    offset=0x407138D0 - BASE,
                    size=4,
                    stock_hex=sp_fit['stock_hex'],
                    v040_v050_nfp_hex=v040_playback_fit_hex,
                    nfp_hex=sp_fit['new_hex'],
                    aspect_fix_hex=aspect_fix_hex,
                    note='Upstream v0.5.1 branches 0x407138d0 -> 0x40968400 (evf_fit_hook). Stage 0 aspect_fix (b 0x4019ffc0 -> 0x409708f0) is preserved if installed: with Pool #6 = 0x001C2000 (1280x960x1.5), all local LCD/EVF frames fit in one slot without needing evf_fit_hook downscaling (which would shrink non-OpenGate Live Photo 1280x848 EVF playback).',
                ),
                pool6_slot_size=dict(
                    va='0x40cf4620',
                    hi_va='0x40cf4630',
                    offset=0x40CF4620 - BASE,
                    size=20,
                    stock_slot='0x00151800',
                    nfp_v051_slot='0x0018d800',
                    v71_stage0_slot='0x001c2000',
                    stock_hex=sp_pool6['stock_hex'],
                    nfp_v051_hex=sp_pool6['new_hex'],
                    v71_stage0_hex=pool6_stage0_hex,
                    middle_12b_hex=pool6_middle,
                    note='Preserves any existing MOVS Pool #6 slot size >= 0x0018D800 (specifically Stage 0 5b 0x001C2000); raises stock 0x00151800 -> 0x0018D800 on standalone stock builds.',
                ),
                loader_copy_end=dict(
                    va='0x41184d8c',
                    offset=0x41184D8C - BASE,
                    size=8,
                    stock_end='0x408bf8f0',
                    min_end='0x40b50000',
                    stage0_c64_end='0x40b8a000',
                    stage3_font_end='0x40c00000',
                    note='All relocated NFP v0.5.1 caves sit strictly below 0x40b50000 (max 0x40b488d0). Preserves any existing loader_copy_end >= 0x40b50000.',
                ),
            ),
            revert_v040=revert_v040,
            apply_v050=apply_windows,
        )
        (HERE / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
        print(
            f"Wrote {HERE / 'v050_bundle.bin'} ({len(z_blob)} B, sha256={sha(z_blob)}) "
            f"and {HERE / 'manifest.json'} ({(HERE / 'manifest.json').stat().st_size} B, "
            f"sha256={sha((HERE / 'manifest.json').read_bytes())})"
        )
        print(json.dumps({
            'target_tag': 'v0.5.1',
            'target_commit': v051_commit,
            'revert_v040_entries': len(revert_v040),
            'apply_v050_entries': len(apply_windows),
            'v050_to_v051_deltas': v050_delta_count,
            'reloc_on_stock_main_sha256': sha(reloc_v051),
            'bundle_zlib_bytes': len(z_blob),
            'bundle_zlib_sha256': sha(z_blob),
            'manifest_sha256': sha((HERE / 'manifest.json').read_bytes()),
        }, indent=2))


if __name__ == '__main__':
    main()
