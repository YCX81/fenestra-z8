#!/usr/bin/env python3
"""Standalone from-stock builder for Nikon Z8 C3.11 V71.7-r7 (v71r7-lut-preview).

Reconstructs the complete V71.7-r7 firmware (`main.main` and `Z_8_0406.bin`) on a
single Linux/WSL machine starting ONLY from the user's official Nikon Z8 C3.11
stock firmware (`Z_8_0311.bin`) and the public `fenestra-z8` + `z8-qemu` source
repositories. Zero private intermediate `.main` images and zero remote SSH hosts
are required.

Usage:
  python3 research/z8-live/from-stock-v71r7/build.py \\
      --stock /path/to/Z_8_0311.bin \\
      --qemu-root /path/to/z8-qemu \\
      --output-dir /path/to/out_v71r7

Optional flags:
  --font /path/to/msyh.ttc        # CJK TTF/TTC font for Stage 3 FontCJK-ZhUI
                                  # (auto-detects WSL /mnt/c/Windows/Fonts/msyh.ttc
                                  #  or system Noto Sans CJK if omitted)
  --skip-fontzh                   # Skip Stage 3 FontCJK-ZhUI (English UI only)
  --no-xm6                        # Skip CEVA-XM6 DSP overlay (100% clang-only ARM NEON)
  --main-only                     # Emit main.main only (skip Z_8_0406.bin packing)
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ASSETS = HERE / 'assets'
ARCHIVE = HERE.parent
REPO = ARCHIVE.parents[1]
FROM_STOCK_V36 = ARCHIVE / 'from-stock'
LIVEDIAG = ARCHIVE / 'build-tools/livediag'
RELEASE = LIVEDIAG / 'feat-release'
V71_ROOT = LIVEDIAG / 'feat-onemov/v71r7-lut-preview'
V71_WORK = V71_ROOT / 'work'
V71_BUILD = V71_ROOT / 'build'

BASE = 0x40160000
BUILD_ID_VA = 0x401FE060
ID_OFF = BUILD_ID_VA - BASE

STOCK_PKG_SHA = 'd67413c2ede6b9ce8887175f20700bd9fbad2a973a4820968c45503d15d80d96'
STOCK_MAIN_SHA = '2b43a31c9de838f84b12416de4348a7004e65be07f853ec56ec2b10f3a8e3853'
CORE_CODE_SHA = 'bc01ea90f042e036d2894879156c33f80873e8ad472295ebfe89dc9275c642a6'
RUNTIME_ID = 'a45bb382924140bcc2c3655e777ae1138927c51a09f25ffdef618fb5766b7577'
EXPECTED_SEED_SHA = '50f2a5b22217710c6f35faf1e4639ef675f2738184525be0190f36bcc42cb36b'
EXPECTED_STAGE0_SHA = 'ffed106403d0e7367a045b3ad3b99512b17f8cb1bffd010398c4fa2df823f398'
EXPECTED_R7_MAIN_SHA = '362ee2a5491859a762b35ed8459cf22c212ae5babb38915daa15e3b801533c8a'
EXPECTED_R7_PKG_SHA = 'eca451be7160945c098e2641f9daf11b9c6755aa4b8ab5c30bdcdd228a8ecfe4'

DEPENDENCIES = {
    'r1': ('v0.3.0-r1', 1022549,
           '704dbb2e09abed4e641174810ad655a413a2eff4042845371499c896634591ed'),
    'rc2': ('v0.4.0-rc.2', 8046605,
            '9a00183b54ee80e02e89445a59f5806a1e738ea22e2400538221ca27c21d9b4c'),
}

HIST_CODE_V50 = bytes.fromhex(
    '8b04003403c9fd9740040035098a9fd28965a0f24901c0f222f9443980038e52'
    'c055a87281008052e3031f2a8d40ff97f503002a28cd80d2a865a0f24801c0f2'
    '080140391f09007161010054c0b983520040a0723f44ff9708bc8152e855a872'
    '09ba8152e955a8721f0400712801889a0300001408b88152e855a872'
)


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def file_sha(path: Path) -> str:
    h = hashlib.sha256()
    with path.open('rb') as f:
        for blk in iter(lambda: f.read(1024 * 1024), b''):
            h.update(blk)
    return h.hexdigest()


def load_mod(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, str(path))
    assert spec and spec.loader, f'Cannot load module {path}'
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def enc_branch(at: int, target: int, link: bool = False) -> bytes:
    delta = target - at
    assert delta % 4 == 0 and -(1 << 27) <= delta < (1 << 27), (hex(at), hex(target))
    return struct.pack('<I', (0x94000000 if link else 0x14000000) | ((delta // 4) & 0x3FFFFFF))


def stamp_build_id(img: bytearray) -> int:
    struct.pack_into('<I', img, ID_OFF, 0)
    bid = struct.unpack('<I', hashlib.sha256(img).digest()[:4])[0] | 0x01000000
    struct.pack_into('<I', img, ID_OFF, bid)
    return bid


def find_compiler(name: str, major: int) -> tuple[str, str]:
    resolved = shutil.which(name)
    if not resolved:
        raise RuntimeError(f'Missing compiler: {name}')
    resolved = str(Path(resolved).resolve())
    ver = subprocess.check_output([resolved, '--version'], text=True).splitlines()[0]
    if f'clang version {major}.' not in ver:
        raise RuntimeError(f'Expected clang {major}.x for {name}, got: {ver}')
    return resolved, ver


def extract_toolkit(kind: str, supplied: Path | None, out: Path) -> Path:
    tag, size, digest = DEPENDENCIES[kind]
    bundled = FROM_STOCK_V36 / 'third-party' / f'NikonFirmwarePatch-{tag}-toolkit.zip'
    archive = supplied.resolve() if supplied else bundled
    if not archive.is_file():
        raise FileNotFoundError(f'Missing NikonFirmwarePatch toolkit archive: {archive}')
    if archive.stat().st_size != size or file_sha(archive) != digest:
        raise ValueError(f'Toolkit SHA-256 mismatch for {kind}: {archive}')
    dest = out / kind
    dest.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(archive) as zf:
        zf.extractall(dest)
    return dest / f'NikonFirmwarePatch-{tag}'


def combine(stock: bytes, image: bytearray, layer: bytes, label: str) -> None:
    for at, (b, a) in enumerate(zip(stock, layer)):
        if b == a:
            continue
        if image[at] not in (b, a):
            raise ValueError(f'{label} overlaps another layer at {BASE + at:#x}')
        image[at] = a


def find_default_font() -> Path | None:
    candidates = [
        Path('/mnt/c/Windows/Fonts/msyh.ttc'),
        Path('/mnt/c/Windows/Fonts/msyh.ttf'),
        Path('/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc'),
        Path('/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc'),
        Path('/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc'),
        Path('/System/Library/Fonts/PingFang.ttc'),
    ]
    for p in candidates:
        if p.is_file():
            return p
    return None


def assemble_flat(clang: str, src_path: Path, base_addr: int, defs: list[str], tmp: Path) -> bytes:
    obj = tmp / (src_path.stem + '.o')
    elf = tmp / (src_path.stem + '.elf')
    out_bin = tmp / (src_path.stem + '.bin')
    subprocess.run([clang, '--target=aarch64-none-elf', '-c', str(src_path), '-o', str(obj)], check=True)
    subprocess.run(['ld.lld', f'-Ttext={base_addr:#x}', *defs, str(obj), '-o', str(elf)], check=True)
    subprocess.run(['llvm-objcopy', '-O', 'binary', str(elf), str(out_bin)], check=True)
    return out_bin.read_bytes()


def build_cardslot_cave_local(clang: str, out_dir: Path) -> Path:
    """Compile assets/cardslot_v71r4/cardslot.c locally into cave.bin + manifest.json."""
    out_dir.mkdir(parents=True, exist_ok=True)
    cave_base = 0x401FE300
    cave_end = 0x401FEF00
    wrappers = {
        'open':    (0x402DD4F8, 'str x22, [sp, #-0x30]!',     'f60f1df8'),
        'remove':  (0x402DDA4C, 'str x20, [sp, #-0x20]!',     'f40f1ef8'),
        'rename':  (0x402DD9FC, 'stp x21, x20, [sp, #-0x20]!', 'f553bea9'),
        'opendir': (0x402DDC6C, 'stp x21, x20, [sp, #-0x20]!', 'f553bea9'),
        'mkdir':   (0x402DDBDC, 'str x20, [sp, #-0x20]!',     'f40f1ef8'),
        'read':    (0x402DD6C8, 'stp x25, x24, [sp, #-0x40]!', 'f963bca9'),
        'write':   (0x402DD78C, 'sub sp, sp, #0x60',          'ff8301d1'),
    }
    tramp = ['.section .text.tramp,"ax"']
    defs = {}
    for name, (entry, insn, _) in wrappers.items():
        tramp += [f'.globl o_{name}', '.balign 4', f'o_{name}:', '  ' + insn, f'  b BODY_{name}']
        defs['BODY_' + name] = entry + 4
    (out_dir / 'tramp.s').write_text('\n'.join(tramp) + '\n')
    lds = (
        f'SECTIONS {{\n  . = {cave_base:#x};\n'
        '  .text : { *(.text.tramp) *(.text*) *(.rodata*) }\n'
        '  /DISCARD/ : { *(.comment) *(.note*) *(.ARM.attributes) *(.data*) *(.bss*) }\n}\n'
    )
    (out_dir / 'cave.ld').write_text(lds)
    cflags = [
        '--target=aarch64-none-elf', '-Oz', '-ffreestanding', '-fno-builtin',
        '-fno-stack-protector', '-mgeneral-regs-only', '-mstrict-align', '-fno-pic',
        '-fno-asynchronous-unwind-tables', '-fno-unwind-tables', '-mno-outline-atomics',
        '-Wall', '-Wextra', '-Werror',
    ]
    subprocess.run([clang, *cflags, '-c', str(ASSETS / 'cardslot_v71r4/cardslot.c'),
                    '-o', str(out_dir / 'c.o')], check=True)
    subprocess.run([clang, '--target=aarch64-none-elf', '-c', str(out_dir / 'tramp.s'),
                    '-o', str(out_dir / 't.o')], check=True)
    ld_cmd = ['ld.lld', '-T', str(out_dir / 'cave.ld'), '-e', 'hook_open',
              '-o', str(out_dir / 'cave.elf'), str(out_dir / 't.o'), str(out_dir / 'c.o')]
    for k, v in defs.items():
        ld_cmd += ['--defsym', f'{k}={v:#x}']
    subprocess.run(ld_cmd, check=True)
    subprocess.run(['llvm-objcopy', '-O', 'binary', '--only-section=.text',
                    str(out_dir / 'cave.elf'), str(out_dir / 'cave.bin')], check=True)
    cave = (out_dir / 'cave.bin').read_bytes()
    assert len(cave) <= cave_end - cave_base, f'cardslot cave overflow: {len(cave)}'
    nm = subprocess.check_output(['llvm-nm', '--defined-only', str(out_dir / 'cave.elf')], text=True)
    syms = {l.split()[2]: int(l.split()[0], 16) for l in nm.splitlines() if len(l.split()) == 3}
    hooks = {}
    for name, (entry, insn, orig_hex) in wrappers.items():
        target = syms['hook_' + name]
        hooks[name] = {
            'entry': hex(entry),
            'orig_insn': insn,
            'orig_hex': orig_hex,
            'branch': enc_branch(entry, target, link=False).hex(),
            'target': hex(target),
        }
    manifest = {
        'schema': 'v71r4.cardslot/v1',
        'cave_addr': hex(cave_base),
        'cave_ceiling': hex(cave_end),
        'cave_bytes': len(cave),
        'cave_end': hex(cave_base + len(cave)),
        'cave_free_before_font_overlay': (cave_end - cave_base) - len(cave),
        'cave_sha256': sha(cave),
        'hooks': hooks,
        'symbols': {k: hex(v) for k, v in sorted(syms.items())},
    }
    (out_dir / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    return out_dir


def build_nfp_rc2_4group(nfp_pkg: Path, stock_pkg: Path) -> tuple[bytes, bytes]:
    """Return (3-group rc2 MAIN, 4-group rc2 MAIN including log-colour)."""
    sys.path.insert(0, str(nfp_pkg.resolve()))
    try:
        from z8prores.package import Package
        from features import build as nfp_build
        package = Package(stock_pkg.read_bytes())
        eg3, _ = nfp_build(package.eg, '04.06', 'p0',
                           ['framing', 'video-quality', 'photo-tools'], code_source='prebuilt')
        eg4, _ = nfp_build(package.eg, '04.06', 'p0',
                           ['framing', 'video-quality', 'log-colour', 'photo-tools'], code_source='prebuilt')
        return eg3[:-2], eg4[:-2]
    finally:
        if sys.path and sys.path[0] == str(nfp_pkg.resolve()):
            sys.path.pop(0)


def pack_update_container(nfp_pkg: Path, stock_pkg: Path, main_path: Path, out_pkg: Path) -> None:
    """Pack main.main into Z_8_0406.bin using official Z_8_0311.bin XOR envelope tables
    (matching pack68.py: avoids z8prores.Package's offline 0xB10000 zero-window heuristic
    which overlaps CJK_EXT_2 at 0x40B8A000..0x40C00000)."""
    code = (
        "import binascii, hashlib, sys; from pathlib import Path; "
        "sys.path.insert(0, sys.argv[1]); "
        "from z8prores.package import Package, transform, AUTH_APPEND, OUTER_FIELD; "
        "orig = Package(Path(sys.argv[2]).read_bytes()); "
        "m = Path(sys.argv[3]).read_bytes(); "
        "eg = m + binascii.crc_hqx(m, 0).to_bytes(2, 'big'); "
        "assert len(eg) == 81807711 and binascii.crc_hqx(eg, 0) == 0; "
        "body = bytearray(orig.body); "
        "body[112:128] = b'eg2070_0406p0.bi'; "
        "body[581844:581844 + len(eg)] = eg; "
        "body[-16:-14] = binascii.crc_hqx(body[:-16], 0).to_bytes(2, 'big'); "
        "bb = bytes(body); "
        "pkt = hashlib.sha1(bb + AUTH_APPEND).digest() + OUTER_FIELD + transform(bb, orig.tables); "
        "assert len(pkt) == 99051117 and transform(pkt[32:], orig.tables) == bb; "
        "Path(sys.argv[4]).write_bytes(pkt)"
    )
    subprocess.run(
        [sys.executable, '-B', '-c', code, str(nfp_pkg.resolve()),
         str(stock_pkg.resolve()), str(main_path.resolve()), str(out_pkg.resolve())],
        check=True,
    )


def build_stage0(
    stock_pkg: Path,
    qemu_root: Path,
    out: Path,
    core_clang: str,
    live_clang: str,
    packages: dict[str, Path],
    no_xm6: bool,
) -> tuple[Path, Path, Path]:
    """Reconstruct Stage 0 base image (stage0-base.main) cleanly from stock Z_8_0311.bin."""
    print('=== [Stage 0] Reconstructing V36 seed + V50..V71 base layers from stock C3.11 ===', flush=True)
    s0 = out / 'stage0'
    s0.mkdir(parents=True, exist_ok=True)
    backend = FROM_STOCK_V36 / 'nfp_backend.py'

    # 1. Decode stock.main & build NFP r1 / rc2
    stock_path = s0 / 'stock.main'
    subprocess.run([sys.executable, '-B', str(backend), 'decode',
                    '--toolkit', str(packages['rc2']), '--stock', str(stock_pkg),
                    '--output', str(stock_path)], check=True)
    stock = stock_path.read_bytes()
    assert sha(stock) == STOCK_MAIN_SHA, f'Unexpected stock.main SHA: {sha(stock)}'

    nfp_r1_path = s0 / 'nfp-r1.main'
    nfp_rc2_path = s0 / 'nfp-rc2.main'
    subprocess.run([sys.executable, '-B', str(backend), 'r1',
                    '--toolkit', str(packages['r1']), '--stock', str(stock_pkg),
                    '--output', str(nfp_r1_path)], check=True)
    subprocess.run([sys.executable, '-B', str(backend), 'rc2',
                    '--toolkit', str(packages['rc2']), '--stock', str(stock_pkg),
                    '--output', str(nfp_rc2_path)], check=True)

    # 2. Build Fenestra Core (exact V36 core-baseline)
    core_work = s0 / 'core_work'
    core_work.mkdir(parents=True, exist_ok=True)
    for folder in ('host', 'emulation'):
        shutil.copytree(REPO / folder, core_work / folder, ignore=shutil.ignore_patterns('__pycache__'))
    for folder in ('include', 'src'):
        shutil.copytree(ARCHIVE / 'baseline-core' / folder, core_work / folder)
    shutil.copytree(FROM_STOCK_V36 / 'core-overlay', core_work, dirs_exist_ok=True)
    core_build = core_work / 'build/core'
    subprocess.run([sys.executable, '-B', str(core_work / 'host/build_z8_core.py'),
                    '--firmware', str(stock_path), '--output-dir', str(core_build)],
                   cwd=core_work, env=dict(os.environ, V36_CORE_CLANG=core_clang), check=True)
    core_meta = json.loads((core_build / 'build.json').read_text())
    candidate = bytearray((core_build / 'z8-core.main').read_bytes())
    id_at = core_meta['symbols']['fx_z8_debug_build'] - BASE
    candidate[id_at:id_at + 32] = bytes.fromhex(RUNTIME_ID)
    assert core_meta['code_bytes'] == 268776 and sha(candidate[0x40000:0x40000 + 268776]) == CORE_CODE_SHA

    # 3. Compose V36 seed.main (exact 50f2a5b2...)
    helpers = qemu_root / 'experiments/20261111/v36/from-stock'
    sys.path[:0] = [str(helpers), str(qemu_root / 'tools')]
    from z8_slots_image import compose as slots_compose, overlay as slots_overlay, copy_end_bytes, decode_copy_end, COPY_END_SITE
    from z8_slots_layout import Layout
    from z8_orientation_unlock import build_plan

    image = bytearray(nfp_r1_path.read_bytes())
    combine(stock, image, bytes(candidate), 'core')
    orientation = build_plan(stock, video=True, video_portrait=True, video_file_rotation=True)['image']
    combine(stock, image, orientation, 'portrait')
    image, _ = slots_overlay(bytes(image), slots_compose(stock, Layout(64)))
    image = bytearray(image)

    # Legacy movie hook (part of V36 seed.main)
    movie_dir = s0 / 'movie'
    movie_dir.mkdir(exist_ok=True)
    msrc = (FROM_STOCK_V36 / 'legacy/movie_hook.c').read_text()
    for before, after in [('0x408c0024', '0x40a02024'), ('0xa0fec3c00', '0xa10005c00'),
                          ('0xa0fec3c80', '0xa10005c80'), ('0xa0fec4000', '0xa10006000')]:
        msrc = msrc.replace(before, after)
    (movie_dir / 'hook.c').write_text(msrc)
    subprocess.run([core_clang, '--target=aarch64-none-elf', '-ffreestanding', '-fno-builtin',
                    '-fno-stack-protector', '-mgeneral-regs-only', '-fno-pic', '-O1',
                    '-c', str(movie_dir / 'hook.c'), '-o', str(movie_dir / 'hook.o')], check=True)
    subprocess.run(['ld.lld', '-T', str(FROM_STOCK_V36 / 'legacy/movie_hook.ld'), '-e', 'live_entry',
                    str(movie_dir / 'hook.o'), '-o', str(movie_dir / 'hook.elf')], check=True)
    subprocess.run(['llvm-objcopy', '-O', 'binary', str(movie_dir / 'hook.elf'),
                    str(movie_dir / 'hook.bin')], check=True)
    nm_m = subprocess.check_output(['llvm-nm', '--defined-only', str(movie_dir / 'hook.elf')], text=True)
    msyms = {p[2]: int(p[0], 16) for l in nm_m.splitlines() if len(p := l.split()) == 3}
    mcode = (movie_dir / 'hook.bin').read_bytes()
    assert sha(mcode) == '55b7ab9ef88d0a9dbadb033bb5c21fee7a098d534af665c98585e9fa15c6601b'
    image[0x40A02000 - BASE:0x40A02000 - BASE + len(mcode)] = mcode
    for addr, name, call in [(0x4034974C, 'live_entry', True), (0x40FDC9C8, 'live_press_entry', False),
                             (0x40C59D18, 'live_picture_entry', True), (0x40D09710, 'live_take_entry', True),
                             (0x40D09764, 'live_batch_entry', True)]:
        image[addr - BASE:addr - BASE + 4] = enc_branch(addr, msyms[name], link=call)

    # Swap NFP r1 -> rc2 (3-group) to produce exact V36 seed.main (50f2a5b2...)
    old_nfp, new_nfp = nfp_r1_path.read_bytes(), nfp_rc2_path.read_bytes()
    copy_at = COPY_END_SITE - BASE
    copy_end = bytes(image[copy_at:copy_at + 8])
    for at, (ob, oldb) in enumerate(zip(stock, old_nfp)):
        if ob != oldb and not copy_at <= at < copy_at + 8:
            image[at] = ob
    for at, (ob, newb) in enumerate(zip(stock, new_nfp)):
        if ob != newb and not copy_at <= at < copy_at + 8:
            image[at] = newb
    image[copy_at:copy_at + 8] = copy_end
    seed_bytes = bytes(image)
    assert sha(seed_bytes) == EXPECTED_SEED_SHA, f'seed.main mismatch: {sha(seed_bytes)}'
    seed_path = s0 / 'seed.main'
    seed_path.write_bytes(seed_bytes)
    print(f'  [0.1] Reconstructed exact V36 seed.main: {sha(seed_bytes)}', flush=True)

    hooks = [0x403DB694, 0x4032E130, 0x40D60EC8, 0x40FDC9C8, 0x40E90838, 0x40C26804,
             0x40C26818, 0x40C26828, 0x40E8B328, 0x408B7C1C, 0x406CD6A0, 0x406CD75C,
             0x40EA3638, 0x406CD468, 0x41020844, 0x40D60ECC, 0x4085C424, 0x4085BCB0,
             0x401CCE84, 0x401CD3EC, 0x4061A724, 0x40619DE4, 0x40618BE0, 0x40618B84]
    ranges = [dict(address=hex(a), size=0, purpose='empty payload reservation', expected='', replacement='')
              for a in (0x401E2000, 0x40196000)]
    ranges += [dict(address=hex(a), size=4, purpose='source-generated hook preimage',
                    expected=seed_bytes[a - BASE:a - BASE + 4].hex(),
                    replacement=seed_bytes[a - BASE:a - BASE + 4].hex()) for a in hooks]
    seed_manifest_path = s0 / 'seed-manifest.json'
    seed_manifest_path.write_text(json.dumps(dict(
        schema='v36.from-stock-seed/v1', symbols=core_meta['symbols'],
        ranges=ranges, output_sha256=sha(seed_bytes), flashable=False,
    ), indent=2) + '\n')

    # 4. Build V69 pb-fix37 against seed.main, then apply usb_default_on + gui_pool_fix + bootguard_patch
    pb_out = s0 / 'pb'
    env_v37 = dict(os.environ, V36_REAL_CLANG=live_clang,
                   PATH=os.pathsep.join([str(RELEASE / 'tools-v37/compiler'), str(Path(live_clang).parent), os.environ['PATH']]))
    subprocess.run([
        sys.executable, '-B', str(V71_BUILD / 'relive_pb53.py'),
        str(V71_WORK / 'pb-fix37'), str(seed_path), str(seed_manifest_path), str(pb_out),
        '--defs=' + json.dumps({'fx_glyph': '0x401f0000', 'fx_memmove': '0x401f0100'}),
    ], env=env_v37, check=True)
    pb_main = pb_out / 'seed.main'
    usb_dir = s0 / 'usb'
    usb_dir.mkdir(exist_ok=True)
    usb_main = usb_dir / 'usb.main'
    subprocess.run([sys.executable, '-B', str(ARCHIVE / 'build-tools/usb_default_on.py'),
                    str(pb_main), str(usb_main)], check=True)
    chain_mod = load_mod('v36_chain', RELEASE / 'tools-v36/chain.py')
    usb_man = chain_mod.append_changes(
        pb_main.read_bytes(), usb_main.read_bytes(),
        json.loads((pb_main.parent / 'changed-ranges.json').read_text()), 'USB default on',
    )
    (usb_dir / 'changed-ranges.json').write_text(json.dumps(usb_man, indent=2) + '\n')
    pool_dir = s0 / 'pool'
    pool_dir.mkdir(exist_ok=True)
    pool_main = pool_dir / 'pool.main'
    subprocess.run([sys.executable, '-B', str(ARCHIVE / 'build-tools/gui_pool_fix.py'),
                    str(usb_main), str(pool_main)], check=True)
    pb_v37_path = s0 / 'pb_only_v37.main'
    subprocess.run([sys.executable, '-B', str(RELEASE / 'tools-v36/bootguard_patch.py'),
                    str(pool_main), str(pb_v37_path)], env=env_v37, check=True)
    print(f'  [0.2] Built V69 pb-fix37 + usb/pool/bootguard: {file_sha(pb_v37_path)}', flush=True)

    # 5. Apply c64label (0x21f -> 0x229) + c64gate + c64move (0x40B00000 -> 0x40B50000) + NFP rc2 log-colour
    img = bytearray(pb_v37_path.read_bytes())

    # 5a. Compute boot-init blob diffs for c64label (275 descriptors 0x21f -> 0x229) + c64gate (limit 0x40 -> 9)
    old_c64 = slots_compose(stock, Layout(64))
    old_blob_patch = next(p for p in old_c64['patches'] if p['address'] == Layout.boot_init)
    old_bo = old_blob_patch['source_offset']
    blob_copy = bytearray(img[old_bo:old_bo + len(old_blob_patch['replacement'])])
    assert bytes(blob_copy) == old_blob_patch['replacement'], 'V36 C64 boot-init blob mismatch'

    # c64label: 275 u16 descriptors at 0x40b37214 + k*32 (offset inside boot_init blob)
    first_style_off = (0x40B37214 - BASE) - old_bo
    for k in range(275):
        p_off = first_style_off + k * 32
        assert struct.unpack_from('<H', blob_copy, p_off)[0] == 0x21F, (k, hex(p_off))
        struct.pack_into('<H', blob_copy, p_off, 0x229)

    # c64gate: limit word 0x40 -> 9 at RAM 0xa116d3400 inside boot-init span table
    p_tbl = 0x200  # 0x40b20200 - 0x40b20000
    tbl_entries = []
    while True:
        dest, sz = struct.unpack_from('<II', blob_copy, p_tbl)
        p_tbl += 8
        if not sz:
            break
        tbl_entries.append((dest, sz))
    p_data = 0x608  # 0x40b20608 - 0x40b20000
    limit_ram_off = 0xA116D3400 - 0xA1162B000
    limit_patched = False
    for dest, sz in tbl_entries:
        if dest <= limit_ram_off < dest + sz:
            lo = p_data + (limit_ram_off - dest)
            assert struct.unpack_from('<I', blob_copy, lo)[0] == 0x40
            struct.pack_into('<I', blob_copy, lo, 9)
            limit_patched = True
            break
        p_data += sz
    assert limit_patched, 'Could not locate C64 limit word in boot-init blob'
    blob_diff = [(i, old_blob_patch['replacement'][i], blob_copy[i])
                 for i in range(len(blob_copy)) if blob_copy[i] != old_blob_patch['replacement'][i]]

    # 5b. Revert old C64 at 0x40B00000, add NFP rc2 log-colour, install moved C64 at 0x40B50000
    for p in old_c64['patches']:
        o, e, r = p['source_offset'], p['expected'], p['replacement']
        assert bytes(img[o:o + len(r)]) == r or p['address'] == Layout.boot_init, hex(p['address'])
        img[o:o + len(e)] = e

    three_rc2, four_rc2 = build_nfp_rc2_4group(packages['rc2'], stock_pkg)
    for at in range(len(img)):
        if three_rc2[at] != four_rc2[at] and not copy_at <= at < copy_at + 8:
            assert img[at] == three_rc2[at], f'log-colour collision at {BASE + at:#x}'
            img[at] = four_rc2[at]

    SHIFT = 0x50000
    class Moved(Layout):
        core_code = Layout.core_code + SHIFT
        main_code = Layout.main_code + SHIFT
        media_code = Layout.media_code + SHIFT
        label_code = Layout.label_code + SHIFT
        imenu_code = Layout.imenu_code + SHIFT
        code_base = Layout.code_base + SHIFT
        boot_init = Layout.boot_init + SHIFT

    new_c64 = slots_compose(stock, Moved(64))
    for p in new_c64['patches']:
        o, e, n = p['source_offset'], p['expected'], p['replacement']
        assert bytes(img[o:o + len(e)]) == e, f'moved C64 target not free at {hex(p["address"])}'
        img[o:o + len(n)] = n
    new_blob = next(p for p in new_c64['patches'] if p['address'] == Moved.boot_init)
    nbo = new_blob['source_offset']
    for i, was, now in blob_diff:
        assert new_blob['replacement'][i] == was
        img[nbo + i] = now

    # Install c64gate cave at 0x401fe2c0 targeting moved invalidate (0x40b53400) + SLOTS_LIMIT movk
    gate_cave = 0x401FE2C0
    inv_moved = 0x40B03400 + SHIFT
    gate_asm = f"""
.globl _start
_start:
        movz x9, #{0xA116D3400 & 0xFFFF:#x}
        movk x9, #{(0xA116D3400 >> 16) & 0xFFFF:#x}, lsl #16
        movk x9, #{(0xA116D3400 >> 32) & 0xFFFF:#x}, lsl #32
        ldr w10, [x9]
        cmp w10, #32
        b.hs 1f
        mov w10, #64
        str w10, [x9]
        dmb ish
        bl INVALIDATE
1:      mov w8, #0x40
        b INFO_NEXT
"""
    with tempfile.TemporaryDirectory() as td:
        gpath = Path(td) / 'gate.s'
        gpath.write_text(gate_asm)
        gcode = assemble_flat(live_clang, gpath, gate_cave,
                              [f'--defsym=INVALIDATE={inv_moved:#x}', '--defsym=INFO_NEXT=0x401a74a0'], Path(td))
    assert len(gcode) <= 0x40 and not any(img[gate_cave - BASE:gate_cave - BASE + len(gcode)])
    img[gate_cave - BASE:gate_cave - BASE + len(gcode)] = gcode
    assert img[0x401A749C - BASE:0x401A749C - BASE + 4].hex() == '08088052'
    img[0x401A749C - BASE:0x401A749C - BASE + 4] = enc_branch(0x401A749C, gate_cave, link=False)
    lim_off = 0x401A769C - BASE
    assert bytes(img[lim_off - 4:lim_off + 8]) == bytes.fromhex('088086520816a87200013fd6')
    struct.pack_into('<I', img, lim_off, 0x72A00008 | ((((inv_moved) >> 16) & 0xFFFF) << 5))
    end_addr = max(new_c64['copy_end'], decode_copy_end(four_rc2[copy_at:copy_at + 8]))
    img[copy_at:copy_at + 8] = copy_end_bytes(end_addr)
    print(f'  [0.3] Applied c64label + c64gate + c64move (0x40B50000) + NFP rc2 log-colour', flush=True)

    # 5c. Apply portrait_gate (0x401fe200 cave + 3 hooks) & osd_tableb_fix (table B := table A, 0x1f41 rect)
    pg_mod = load_mod('portrait_gate_patch', ASSETS / 'legacy_steps/portrait_gate_patch.py')
    for pname, (pa, phx) in pg_mod.SITES.items():
        assert img[pa - BASE:pa - BASE + 4].hex() == phx, (pname, hex(pa))
    assert not any(img[pg_mod.CAVE - BASE:pg_mod.CAVE - BASE + 0xC0])
    pg_defs = {k: v[0] for k, v in pg_mod.SITES.items()}
    with tempfile.TemporaryDirectory() as td:
        tdp = Path(td)
        (tdp / 'pg.s').write_text(pg_mod.CAVE_ASM)
        subprocess.run([live_clang, '--target=aarch64-none-elf', '-c', str(tdp / 'pg.s'), '-o', str(tdp / 'pg.o')], check=True)
        ldc = ['ld.lld', f'-Ttext={pg_mod.CAVE:#x}', '-e', '_start', '-o', str(tdp / 'pg.elf'), str(tdp / 'pg.o')]
        for k, v in pg_defs.items():
            ldc += ['--defsym', f'{k}={v:#x}']
        subprocess.run(ldc, check=True)
        nm_pg = subprocess.check_output(['llvm-nm', str(tdp / 'pg.elf')], text=True)
        pg_syms = {l.split()[2]: int(l.split()[0], 16) for l in nm_pg.splitlines() if len(l.split()) == 3}
        subprocess.run(['llvm-objcopy', '-O', 'binary', '--only-section=.text', str(tdp / 'pg.elf'), str(tdp / 'pg.bin')], check=True)
        pg_cave = (tdp / 'pg.bin').read_bytes()
    assert len(pg_cave) <= 0xC0
    img[pg_mod.CAVE - BASE:pg_mod.CAVE - BASE + len(pg_cave)] = pg_cave
    img[pg_mod.SITES['SVC_BRANCH'][0] - BASE:pg_mod.SITES['SVC_BRANCH'][0] - BASE + 4] = pg_mod.branch(pg_mod.SITES['SVC_BRANCH'][0], pg_syms['svc'], cond=1)
    img[pg_mod.SITES['TKHD_POSE'][0] - BASE:pg_mod.SITES['TKHD_POSE'][0] - BASE + 4] = pg_mod.branch(pg_mod.SITES['TKHD_POSE'][0], pg_syms['tkhd'])
    img[pg_mod.SITES['POLICY_AND'][0] - BASE:pg_mod.SITES['POLICY_AND'][0] - BASE + 4] = pg_mod.branch(pg_mod.SITES['POLICY_AND'][0], pg_syms['policy'])

    # osd_tableb_fix: table B := table A, and 0x1f41 portrait rect := (252,368,414,450) in both tables
    osd_a, osd_b, osd_n, osd_r = 0x43A21A70, 0x43A3CE90, 644, 68
    ta = bytearray(img[osd_a - BASE:osd_a - BASE + osd_n * osd_r])
    for idx in range(osd_n):
        if struct.unpack_from('<I', ta, idx * osd_r + 56)[0] == 0x1F41:
            struct.pack_into('<4h', ta, idx * osd_r + 32, 252, 368, 414, 450)
    img[osd_a - BASE:osd_a - BASE + osd_n * osd_r] = ta
    img[osd_b - BASE:osd_b - BASE + osd_n * osd_r] = ta
    print('  [0.3b] Applied portrait_gate (0x401fe200) + osd_tableb_fix', flush=True)

    # 6. Apply V50 Portrait (video_layout_v2.py + HIST_CODE) + liveplan.S + livefbh.S + V51d meta_handler
    v50_layout = load_mod('v50_layout', qemu_root / 'experiments/20261114/portrait-full/video_layout_v2.py')
    for address, fields, landscape, _ in v50_layout.rows(stock):
        if any(fields[8:10]) or not any(fields[6:8]):
            continue
        new_rect = v50_layout.portrait_rect(fields[14], landscape)
        packed = struct.pack('<4h', *new_rect)
        for delta in (0, 0x1B420):
            at = address + delta + 32 - BASE
            img[at:at + 8] = packed
    hist_s, hist_e = 0x4041B248 - BASE, 0x4041B2C4 - BASE
    assert bytes(img[hist_s:hist_e]) == stock[hist_s:hist_e]
    img[hist_s:hist_e] = HIST_CODE_V50

    with tempfile.TemporaryDirectory() as td:
        tdp = Path(td)
        for src_name, cave, hook, orig_hex, is_bl, defs in [
            ('liveplan.S', 0x401FD000, 0x402F3264, '092c4029', False, ['--defsym=fw_resume=0x402f3268']),
            ('livefbh.S',  0x401FD100, 0x402F3C54, '086d1c12', True,  []),
        ]:
            code = assemble_flat(live_clang, LIVEDIAG / 'feat-v50/playback' / src_name, cave, defs, tdp)
            assert img[hook - BASE:hook - BASE + 4].hex() == orig_hex, src_name
            assert len(code) <= 0x100 and not any(img[cave - BASE:cave - BASE + len(code)])
            img[cave - BASE:cave - BASE + len(code)] = code
            img[hook - BASE:hook - BASE + 4] = enc_branch(hook, cave, link=is_bl)

        # V51d meta_handler_patch (0x401fd200, hook 0x40cea514)
        meta_asm = '.text\nmov w10, #0x7461\nmovk w10, #0x6d65, lsl #16\ncmp w0, w10\nmov w0, #6\ncsel w0, wzr, w0, eq\nret\n'
        mp = tdp / 'meta.s'
        mp.write_text(meta_asm)
        mcode2 = assemble_flat(live_clang, mp, 0x401FD200, [], tdp)
        assert img[0x40CEA514 - BASE:0x40CEA514 - BASE + 4].hex() == 'e0071f32'
        assert not any(img[0x401FD200 - BASE:0x401FD200 - BASE + len(mcode2)])
        img[0x401FD200 - BASE:0x401FD200 - BASE + len(mcode2)] = mcode2
        img[0x40CEA514 - BASE:0x40CEA514 - BASE + 4] = enc_branch(0x40CEA514, 0x401FD200, link=True)
    print(f'  [0.4] Applied V50 portrait + liveplan/livefbh + V51d meta_handler', flush=True)

    # 7. Apply V52 core_output_patch + apply_portrait52_final + V53 portrait53 + crop_fix/aspect_fix
    sys.path.insert(0, str(LIVEDIAG / 'feat-v52'))
    from core_output_patch import apply as apply_evf
    pb_symbols = json.loads((pb_out / 'changed-ranges.json').read_text())['symbols']
    img_bytes, _ = apply_evf(img, pb_symbols)
    img = bytearray(img_bytes)

    # Portrait 52 (LCD + EVF): run on current pre-portrait52 image (bypassing historical v51d SHA pin)
    p52_dir = LIVEDIAG / 'feat-v52/portrait-overlay'
    sys.path.insert(0, str(p52_dir))
    import apply_portrait52 as ap52_mod
    import lcd_layout as lcd52_mod
    pre_p52 = bytes(img)
    ap52_mod.PIN = sha(pre_p52)
    p52_edits = ap52_mod.make_edits(pre_p52)
    for x in lcd52_mod.plan(pre_p52):
        a = x['address']
        old_rec = pre_p52[a - BASE:a - BASE + 68]
        new_rec = bytearray(old_rec)
        struct.pack_into('<4h', new_rec, 32, *x['new'])
        p52_edits.append(dict(x, address=hex(a), expected=old_rec.hex(), replacement=bytes(new_rec).hex()))
    for e in p52_edits:
        o = int(e['address'], 16) - BASE
        assert bytes(img[o:o + 68]) == bytes.fromhex(e['expected']), e['address']
        img[o:o + 68] = bytes.fromhex(e['replacement'])

    # Portrait 53: run on post-portrait52 image (bypassing historical v52g SHA pin)
    p53_dir = LIVEDIAG / 'feat-v53/portrait'
    sys.path.insert(0, str(p53_dir))
    import portrait53 as p53_mod
    pre_p53 = bytes(img)
    p53_mod.PIN = sha(pre_p53)
    img_p53, _ = p53_mod.apply(pre_p53, pre_p53)
    img = bytearray(img_p53)

    # Compile & install crop_fix.S (0x4019ff40, 128 B) and aspect_fix.S (0x4019ffc0, 64 B)
    crop_dir = LIVEDIAG / 'feat-v53/native-rec/crop-fix'
    with tempfile.TemporaryDirectory() as td:
        tdp = Path(td)
        for name, addr, max_sz, hook, exp_bytes in [
            ('crop_fix',   0x4019FF40, 128, 0x40CF7D08, bytes.fromhex('692a0079')),
            ('aspect_fix', 0x4019FFC0, 64,  0x407138D0, enc_branch(0x407138D0, 0x409708F0, link=False)),
        ]:
            payload = assemble_flat(live_clang, crop_dir / f'{name}.S', addr, [], tdp)
            assert len(payload) <= max_sz, (name, len(payload))
            at = addr - BASE
            assert not any(img[at:at + max_sz]), f'{name} cave not zero'
            img[at:at + len(payload)] = payload
            hat = hook - BASE
            assert bytes(img[hat:hat + 4]) == exp_bytes, f'{name} hook mismatch at {hook:#x}'
            img[hat:hat + 4] = enc_branch(hook, addr, link=False)
    print(f'  [0.5] Applied V52/V53 portrait overlays + crop_fix/aspect_fix', flush=True)

    # 8. V71 Stage 0 overlays: Bug #2 EVF Pool #6 + cardslot_v71r4 + C64 i-menu UI + NFP v0.5.1 + XM6 DSP
    for addr, old_w, new_w in [(0x40CF4620, 0x5283000D, 0x5284000D), (0x40CF4630, 0x72A002AD, 0x72A0038D)]:
        off = addr - BASE
        assert struct.unpack_from('<I', img, off)[0] == old_w
        struct.pack_into('<I', img, off, new_w)

    cardslot_dir = build_cardslot_cave_local(live_clang, s0 / 'cardslot_v71r4')
    sys.path.insert(0, str(V71_BUILD))
    import patch_boot_panic03_cardslot
    patch_boot_panic03_cardslot.apply(img, cardslot_dir=cardslot_dir)

    import patch_c64_imenu_ui
    patch_c64_imenu_ui.apply(img)

    import patch_nfp_v050
    patch_nfp_v050.apply(img, bundle_dir=ASSETS / 'nfp_v050')

    if not no_xm6:
        import patch_xm6_elf0_stage0
        xm6_meta = patch_xm6_elf0_stage0.apply(img, payload_dir=ASSETS / 'xm6_elf0/coexist_nikondma')
        patch_xm6_elf0_stage0.verify(img, xm6_meta)
        print('  [0.6] Applied EVF Pool #6 + cardslot_v71r4 + C64 UI + NFP v0.5.1 + CEVA-XM6 DSP', flush=True)
    else:
        print('  [0.6] Applied EVF Pool #6 + cardslot_v71r4 + C64 UI + NFP v0.5.1 (--no-xm6 ARM-only)', flush=True)

    bid = stamp_build_id(img)
    stage0_path = s0 / 'stage0-base.main'
    stage0_path.write_bytes(img)
    s0_sha = sha(bytes(img))
    print(f'  [Stage 0 DONE] stage0-base.main SHA-256 = {s0_sha} (build_id={bid:#010x})', flush=True)
    if not no_xm6 and s0_sha == EXPECTED_STAGE0_SHA:
        print('  [EXACT MATCH] stage0-base.main matches pinned V71.7-r7 Stage 0 byte-for-byte!', flush=True)
    return stage0_path, seed_manifest_path, stock_path


def build_stage1(stage0_path: Path, seed_manifest_path: Path, out: Path, live_clang: str) -> tuple[Path, Path, Path]:
    """Compile all 54 V71.7-r7 EL1 units locally and patch stage0-base.main -> stage1.main."""
    print('=== [Stage 1] Compiling 54 V71.7-r7 EL1 units (49 scalar + 5 NEON) ===', flush=True)
    s1 = out / 'stage1'
    obj = s1 / 'objects'
    obj.mkdir(parents=True, exist_ok=True)

    src_fix37 = V71_WORK / 'src-fix37'
    neon_scale = V71_WORK / 'neon_scale'
    filmsim_dir = V71_WORK / 'filmsim'
    filmsim_src = filmsim_dir / 'src'
    lut_core_src = V71_WORK / 'lut_core'
    lut_preview_src = V71_WORK / 'lut_preview'
    lut_menu_src = V71_WORK / 'lut_menu'
    fenestra_inc = REPO / 'include'

    # 1. Build FilmSim EL1 objects (15 units via build_filmsim_objs.sh)
    env_fs = dict(
        os.environ,
        FILMSIM_SRC=str(filmsim_src),
        SRC_FIX37_DIR=str(src_fix37),
        LUT_CORE_DIR=str(lut_core_src),
        LUT_PREVIEW_DIR=str(lut_preview_src),
        LUT_MENU_DIR=str(lut_menu_src),
        FENESTRA_INC_DIR=str(fenestra_inc),
        OUT_DIR=str(obj),
        CC=live_clang,
    )
    subprocess.run(['bash', str(filmsim_dir / 'build_filmsim_objs.sh')], env=env_fs, check=True)

    # 2. Build NEON Direct NV12/I420 Scaler
    subprocess.run([
        live_clang, '--target=aarch64-none-elf', '-O2', '-std=c11',
        '-ffreestanding', '-fno-builtin', '-fno-stack-protector',
        '-fno-unwind-tables', '-fno-asynchronous-unwind-tables',
        '-ffunction-sections', '-fdata-sections', '-mstrict-align', '-mno-outline-atomics',
        '-Wall', '-Wextra', '-Werror',
        '-I', str(neon_scale), '-I', str(src_fix37 / 'capture'),
        '-c', str(neon_scale / 'live_scale_neon.c'),
        '-o', str(obj / 'filmsim_19-live_scale_neon.c.o'),
    ], check=True)

    # 3. Build 21 src-fix37 scalar units
    live_units = [
        'runtime.c', 'hooks.S',
        'capture/live_capture.c', 'capture/native_capture.c', 'capture/live_stream.c',
        'ui/live_ui.c',
        'codec/live_w5enc.c', 'codec/live_h264_idr.c', 'codec/live_mov.c', 'codec/live_jpeg.c',
        'codec/live_transaction.c', 'codec/native_files.c', 'codec/au_ring.c',
        'codec/preview_writer.c', 'codec/preview_reader.c', 'codec/live_hevc.c',
        'audio/live_audio.c', 'audio/live_audio_mov.c', 'audio/live_audio_native.c',
        'audio/live_audio_z8.c', 'audio/live_shutter_z8.c',
    ]
    live_objs = []
    for rel in live_units:
        o = obj / ('live_' + rel.replace('/', '_') + '.o')
        live_objs.append(o)
        subprocess.run([
            live_clang, '--target=aarch64-none-elf', '-Os', '-ffreestanding', '-fno-builtin',
            '-fno-stack-protector', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables',
            '-ffunction-sections', '-fdata-sections', '-mgeneral-regs-only', '-mstrict-align',
            '-DLA_WIDTH=1024', '-DLA_HEIGHT=680', '-DLA_RING_CAPACITY=96', '-DLA_SAMPLE_INTERVAL_US=25000',
            '-DLIVE_BITRATE=8000000', '-DLIVE_IDR_US=500000', '-DLIVE_ENCODE_SPEED=9',
            '-DLIVE_FILMSIM_POOL_HOOKS=1', '-Wall', '-Wextra', '-Werror',
            '-I', str(neon_scale), '-I', str(src_fix37), '-I', str(src_fix37 / 'ui/core/include'),
            '-I', str(src_fix37 / 'codec/freestanding'),
            '-I', str(filmsim_src / 'runtime'), '-I', str(filmsim_src / 'include'),
            '-c', str(src_fix37 / rel), '-o', str(o),
        ], check=True)
    filmsim_objs = sorted(obj.glob('filmsim_*.o'))
    assert len(live_objs) == 21 and len(filmsim_objs) == 16

    # 4. Build V71.7 Bank 1 (12 units) & Bank 2 (5 units)
    b1_common = [
        '--target=aarch64-none-elf', '-std=c11', '-ffreestanding', '-fno-builtin',
        '-fno-stack-protector', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables',
        '-ffunction-sections', '-fdata-sections', '-mstrict-align', '-mno-outline-atomics',
        '-Wall', '-Wextra', '-Werror',
        '-I', str(lut_core_src), '-I', str(lut_preview_src),
        '-I', str(src_fix37 / 'codec/freestanding'),
        '-I', str(filmsim_src / 'include'), '-I', str(filmsim_src / 'xm6'),
        '-I', str(filmsim_src / 'photo'), '-I', str(filmsim_src / 'pipeline'),
    ]
    b1_objs = []
    for rel in ('cube.c', 'cube_frame.c', 'layer0_color.c', 'neon_sentinel.c',
                'photo_flight.c', 'smp.c', 'lut_runtime.c'):
        o = obj / f'v71r7_b1_{rel}.o'
        b1_objs.append(o)
        subprocess.run([live_clang, '-Os', '-mgeneral-regs-only', *b1_common,
                        '-c', str(lut_core_src / rel), '-o', str(o)], check=True)
    for rel in ('cube_frame_neon.c', 'layer0_color_neon.c'):
        o = obj / f'v71r7_b1_{rel}.o'
        b1_objs.append(o)
        subprocess.run([live_clang, '-O2', *b1_common,
                        '-c', str(lut_core_src / rel), '-o', str(o)], check=True)
    for rel in ('photo_workspace.c', 'layer0_provider.c'):
        o = obj / f'v71r7_b1_{rel}.o'
        b1_objs.append(o)
        subprocess.run([live_clang, '-Oz', '-mgeneral-regs-only', *b1_common,
                        '-c', str(lut_preview_src / rel), '-o', str(o)], check=True)
    o_hooks = obj / 'v71r7_b1_lut_hooks.S.o'
    b1_objs.append(o_hooks)
    subprocess.run([live_clang, '--target=aarch64-none-elf', '-c', str(lut_preview_src / 'lut_hooks.S'),
                    '-o', str(o_hooks)], check=True)

    b2_flags = [
        '--target=aarch64-none-elf', '-Oz', '-std=c11', '-ffreestanding', '-fno-builtin',
        '-fno-pic', '-fno-stack-protector', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables',
        '-ffunction-sections', '-fdata-sections', '-mgeneral-regs-only', '-mstrict-align',
        '-mno-outline-atomics', '-Wall', '-Wextra', '-Werror',
        '-I', str(lut_core_src), '-I', str(lut_menu_src),
    ]
    b2_objs = []
    for rel in ('photo_catalog.c', 'photo_journal.c', 'photo_ui.c', 'photo_worker.c', 'photo_menu_ro_tables.c'):
        o = obj / f'v71r7_b2_{rel}.o'
        b2_objs.append(o)
        subprocess.run([live_clang, *b2_flags, '-c', str(lut_menu_src / rel), '-o', str(o)], check=True)

    # 5. Link unified 54-unit payload.elf
    seed_manifest = json.loads(seed_manifest_path.read_text())
    seed_ranges = {int(r['address'], 16): r for r in seed_manifest['ranges']}
    seed_syms = seed_manifest['symbols']
    sym_int = lambda v: int(v, 16) if isinstance(v, str) else int(v)

    def seed_target(site: int) -> int:
        w = struct.unpack('<I', bytes.fromhex(seed_ranges[site]['expected']))[0]
        return site + ((((w & 0x3FFFFFF) ^ 0x2000000) - 0x2000000) * 4)

    exports = [
        'fx_memset', 'fx_native_menu_build', 'fx_native_menu_link',
        'fx_native_display_attach_global', 'fx_native_display_detach',
        'fx_native_display_is_current', 'fx_native_display_present',
        'memcpy', 'memmove', 'memset',
    ]
    defs = {k: sym_int(seed_syms[k]) for k in exports}
    defs['memcmp'] = sym_int(seed_syms['fx_memcmp'])
    defs.update(
        live_old_menu_page=seed_target(0x403DB694),
        live_old_menu_text=seed_target(0x4032E130),
        live_old_system_dispatch=seed_target(0x40D60EC8),
        live_old_movie_press=seed_target(0x40FDC9C8),
        live_irq_resume=0x40E9083C,
        live_old_writer=seed_target(0x40EA3638),
        live_closed_resume=0x408B7C20,
        live_start_resume=0x40C26808,
        live_stop_resume=0x40C2681C,
        live_hold_resume=0x40C2682C,
        live_old_pool=seed_target(0x40E8B328),
        live_w5_bringup_resume=0x402F2D5C,
        live_stock_shutter_prepare=0x40F9CCCC,
        live_stock_shutter_fire=0x40F9CD54,
    )
    filmsim_inc_body = (filmsim_dir / 'filmsim_section.ld.inc').read_text()
    ld_body = """
ENTRY(live_ui_system_dispatch)
SECTIONS {
  .live_shutter 0x4019f020 : { *(EXCLUDE_FILE(*filmsim_*.o *v71r7_b1_*.o *v71r7_b2_*.o) .live_shutter) }
  ASSERT(. <= 0x4019ff40, "Live shutter bank exhausted")

  . = 0x401e2000;
  .text : { *(EXCLUDE_FILE(*filmsim_*.o *v71r7_b1_*.o *v71r7_b2_*.o) .text*) }
  .rodata : { *(EXCLUDE_FILE(*filmsim_*.o *v71r7_b1_*.o *v71r7_b2_*.o) .rodata*) }
  live_primary_end = .;
  ASSERT(live_primary_end <= 0x401fcd00, "live_primary collides with PortraitPB cave 0x401FCD00")

  .live_extra 0x401fd218 : { *(EXCLUDE_FILE(*filmsim_*.o *v71r7_b1_*.o *v71r7_b2_*.o) .live_extra) }
  ASSERT(. <= 0x401fe000, "Live extra cave exhausted")

  .v71r7_lut_menu 0x40981000 : {
    *v71r7_b2_*.o(.text*)
    *(.v71r7_lut_menu .v71r7_lut_menu.*)
    *(.filmsim.pw)
    . = ALIGN(16);
    *v71r7_b2_*.o(.rodata*)
  }
  ASSERT(. <= 0x40990000, "Bank 2 v71r7_lut_menu exhausted")

__FILMSIM__

  .v71r7_lut_core 0x409db000 : {
    *v71r7_b1_*.o(.text*)
    *filmsim_20-z8_xm6_bridge.c.o(.text*)
    *(.v71r7_lut_core .v71r7_lut_core.*)
    . = ALIGN(16);
    *v71r7_b1_*.o(.rodata*)
    *filmsim_20-z8_xm6_bridge.c.o(.rodata*)
  }
  ASSERT(. <= 0x409ec000, "Bank 1 v71r7_lut_core exhausted")

  .data : { *(.data*) }
  .bss : { *(.bss*) *(COMMON) }
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
""".replace('__FILMSIM__', filmsim_inc_body)
    ld_path = s1 / 'payload.ld'
    ld_path.write_text('\n'.join(f'{k} = {v:#x};' for k, v in defs.items()) + ld_body)
    elf_path = s1 / 'payload.elf'
    subprocess.run(['ld.lld', '-T', str(ld_path), *map(str, live_objs), *map(str, filmsim_objs),
                    *map(str, b1_objs), *map(str, b2_objs), '-o', str(elf_path)], check=True)
    undef = [l for l in subprocess.check_output(['llvm-nm', '-u', str(elf_path)], text=True).splitlines() if l.strip()]
    assert not undef, f'Undefined symbols: {undef}'

    # 6. Extract 6 RX banks + symbol table from ELF
    elf_bytes = elf_path.read_bytes()
    shoff = struct.unpack_from('<Q', elf_bytes, 40)[0]
    _, num, shstrndx = struct.unpack_from('<HHH', elf_bytes, 58)
    hdrs = [struct.unpack_from('<IIQQQQIIQQ', elf_bytes, shoff + i * 64) for i in range(num)]
    shstrs = elf_bytes[hdrs[shstrndx][4]:hdrs[shstrndx][4] + hdrs[shstrndx][5]]
    sec_names = [shstrs[h[0]:].split(b'\0', 1)[0].decode() for h in hdrs]
    bounds = {
        'shutter':  (0x4019F020, 0x4019FF40),
        'primary':  (0x401E2000, 0x401FCD00),
        'extra':    (0x401FD218, 0x401FE000),
        'lut_menu': (0x40981000, 0x40990000),
        'filmsim':  (0x409B0000, 0x409D8000),
        'lut_core': (0x409DB000, 0x409EC000),
    }
    symbols: dict[str, int] = {}
    regions: dict[str, list] = {k: [] for k in bounds}
    for name, h in zip(sec_names, hdrs):
        typ, flags, addr, off, size = h[1:6]
        if typ == 2:
            st = hdrs[h[6]]
            strings = elf_bytes[st[4]:st[4] + st[5]]
            for at in range(off, off + size, 24):
                nm, _, _, idx, value, _ = struct.unpack_from('<IBBHQQ', elf_bytes, at)
                if idx != 0:
                    sname = strings[nm:].split(b'\0', 1)[0].decode()
                    if sname:
                        symbols[sname] = value
        if not (flags & 2) or not size:
            continue
        assert not (flags & 1) and typ != 8, f'Writable alloc section forbidden: {name}'
        for rname, (lo, hi) in bounds.items():
            if lo <= addr and addr + size <= hi:
                regions[rname].append((addr, name, elf_bytes[off:off + size]))
                break
        else:
            raise AssertionError(f'Section {name} @ {addr:#x}+{size} outside allowed banks')

    blobs = {}
    for rname, (lo, hi) in bounds.items():
        rs = sorted(regions[rname])
        data = bytearray()
        cur = lo
        for addr, _, payload in rs:
            data.extend(bytes(addr - cur))
            data.extend(payload)
            cur = addr + len(payload)
        assert cur <= hi
        blobs[rname] = bytes(data)

    # 7. Patch stage0-base.main -> stage1.main
    stage0_bytes = stage0_path.read_bytes()
    img = bytearray(stage0_bytes)
    for rname, (lo, hi) in bounds.items():
        off = lo - BASE
        assert not any(img[off:off + (hi - lo)]), f'Bank {rname} not zero in stage0'
        img[off:off + len(blobs[rname])] = blobs[rname]

    live_sites = [
        (0x403DB694, 'live_ui_menu_page', False), (0x4032E130, 'live_ui_menu_text', False),
        (0x40D60EC8, 'live_ui_system_dispatch', False), (0x40FDC9C8, 'live_press_entry', False),
        (0x40E90838, 'live_irq_entry', False), (0x40C26804, 'live_start_entry', False),
        (0x40C26818, 'live_stop_entry', False), (0x40C26828, 'live_hold_entry', False),
        (0x40E8B328, 'live_pool_wrap', True), (0x408B7C1C, 'live_closed_entry', False),
        (0x406CD6A0, 'live_finalize_wrap', True), (0x406CD75C, 'live_event_wrap', True),
        (0x40EA3638, 'live_writer_wrap', True), (0x406CD468, 'live_open_wrap', True),
        (0x402F2D58, 'live_w5_bringup_entry', False), (0x40F9CB64, 'live_shutter_entry', True),
    ]
    extra_exp = {0x402F2D58: 'ff0302d1', 0x40F9CB64: '08014039'}
    for at, sname, is_call in live_sites:
        exp = extra_exp.get(at) or seed_ranges[at]['expected']
        assert img[at - BASE:at - BASE + 4].hex() == exp, (hex(at), sname)
        img[at - BASE:at - BASE + 4] = enc_branch(at, symbols[sname], link=is_call)

    for at, exp, sname, is_call in [
        (0x40F6CFF0, '37bf0094', 'live_shutter_prepare_entry', True),
        (0x40F6E174, 'f8ba0094', 'live_shutter_fire_entry', True),
        (0x40E30E3C, '3c0f0014', 'live_shutter_dispatch', False),
    ]:
        assert img[at - BASE:at - BASE + 4].hex() == exp, (hex(at), sname)
        img[at - BASE:at - BASE + 4] = enc_branch(at, symbols[sname], link=is_call)

    assert bytes(img[0x401DEF00 - BASE:0x401DEF00 - BASE + 8]) == struct.pack('<Q', 0x403EFED8)
    img[0x401DEF00 - BASE:0x401DEF00 - BASE + 8] = struct.pack('<Q', symbols['live_ui_evf_submit'])

    sys.path.insert(0, str(V71_BUILD))
    import patch_menu_context, patch_review_call, patch_shutter_calls
    pre_menu = bytes(img)
    menu_bytes, _ = patch_menu_context.patch(
        pre_menu, sha(pre_menu),
        dict(live_payload_bytes=len(blobs['primary']), live_free=0x401FD000 - (0x401E2000 + len(blobs['primary']))),
    )
    review_bytes, _ = patch_review_call.patch(menu_bytes, sha(menu_bytes), symbols['live_review_pop'])
    duplex_guards = json.loads((V71_BUILD / 'callsite-guards-r5.json').read_text())
    duplex_bytes, _ = patch_shutter_calls.patch(review_bytes, sha(review_bytes), symbols, duplex_guards)

    hf_spec = json.loads((src_fix37 / 'hangfix_hooks.json').read_text())
    hf_img = bytearray(duplex_bytes)
    for h in hf_spec['hooks']:
        at = int(h['address'], 16)
        target = symbols[h['target_symbol']]
        if h['kind'] == 'PTR64':
            assert hf_img[at - BASE:at - BASE + 8].hex() == h['original_hex']
            hf_img[at - BASE:at - BASE + 8] = struct.pack('<Q', target)
        else:
            assert hf_img[at - BASE:at - BASE + 4].hex() == h['original_hex']
            hf_img[at - BASE:at - BASE + 4] = enc_branch(at, target, link=(h['kind'] == 'BL'))

    sys.path.insert(0, str(filmsim_dir))
    import patch_photo_v69
    photo_bytes, _ = patch_photo_v69.apply(bytes(hf_img), symbols, expected_base_sha256=sha(bytes(hf_img)), require_bank=True)

    v71_img = bytearray(photo_bytes)
    for at, exp, sname in [
        (0x40E9A540, '66f5f897', 'v71r7_layer0_enqueue_hook'),
        (0x40CD862C, 'd779d797', 'v71r7_layer0_display_hook'),
        (0x40CD72A4, 'e7080094', 'v71r7_layer0_vsync_hook'),
        (0x40CD72D4, 'db080094', 'v71r7_layer0_vsync_hook'),
        (0x40D67328, 'dbe6ff97', 'v71r7_power_sync_hook'),
        (0x40D67340, 'd5e6ff97', 'v71r7_power_sync_hook'),
    ]:
        assert v71_img[at - BASE:at - BASE + 4].hex() == exp, (hex(at), sname)
        v71_img[at - BASE:at - BASE + 4] = enc_branch(at, symbols[sname], link=True)

    sys.path.insert(0, str(lut_menu_src))
    import gen_photo_menu_tables
    ram_patches = gen_photo_menu_tables.build_ram_init_patches(
        stage0_bytes,
        parent_desc_va=symbols['v71r7_photo_parent_desc'],
        lut_vector_va=symbols['v71r7_lut_child_vector'],
        lut_desc_va=symbols['v71r7_lut_child_desc'],
    )
    for rp in ram_patches:
        foff = int(rp['file_offset'], 16)
        sz = int(rp['bytes'])
        assert bytes(v71_img[foff:foff + sz]) == bytes.fromhex(rp['old_hex'])
        v71_img[foff:foff + sz] = bytes.fromhex(rp['new_hex'])

    bid = stamp_build_id(v71_img)
    s1_bytes = bytes(v71_img)
    s1_main = s1 / 'stage1.main'
    s1_main.write_bytes(s1_bytes)
    s1_build_json = s1 / 'stage1-build.json'
    s1_ranges_json = s1 / 'stage1-ranges.json'
    s1_build_json.write_text(json.dumps(dict(
        stage1_sha256=sha(s1_bytes), build_id=hex(bid),
        live_payload_bytes=len(blobs['primary']),
        live_free=0x401FD000 - (0x401E2000 + len(blobs['primary'])),
        live_ui_menu_text=hex(symbols['live_ui_menu_text']),
    ), indent=2) + '\n')
    s1_ranges_json.write_text(json.dumps(dict(
        schema='v71.stage1-ranges/v2', stage1_sha256=sha(s1_bytes),
        symbols={k: v for k, v in sorted(symbols.items())
                 if k.startswith(('live_', 'lp_', 'la_', 'pb_', 'z8_', 'filmsim_', 'v71r7_', 'photo_'))},
    ), indent=2) + '\n')
    print(f'  [Stage 1 DONE] stage1.main SHA-256 = {sha(s1_bytes)} (build_id={bid:#010x})', flush=True)
    return s1_main, s1_build_json, s1_ranges_json


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stock', type=Path, required=True, help='Path to official Nikon Z8 C3.11 Z_8_0311.bin')
    parser.add_argument('--qemu-root', type=Path, default=REPO.parent / 'z8-qemu', help='Path to companion z8-qemu repository')
    parser.add_argument('--output-dir', type=Path, required=True, help='Output directory (must not exist or be empty)')
    parser.add_argument('--core-clang', default='clang-18', help='Clang 18 binary for Fenestra Core')
    parser.add_argument('--live-clang', default='clang-21', help='Clang 21 binary for V71.7-r7 payload')
    parser.add_argument('--llvm-bin', type=Path, help='Directory containing LLVM 21 ld.lld / llvm-objcopy / llvm-nm')
    parser.add_argument('--font', type=Path, help='CJK TrueType font (.ttc/.ttf) for Stage 3 FontCJK-ZhUI')
    parser.add_argument('--skip-fontzh', action='store_true', help='Skip Stage 3 FontCJK-ZhUI (English UI only)')
    parser.add_argument('--no-xm6', action='store_true', help='Skip CEVA-XM6 DSP overlay (100%% pure-clang ARM NEON)')
    parser.add_argument('--main-only', action='store_true', help='Skip final Z_8_0406.bin update container packing')
    args = parser.parse_args()

    if os.name != 'posix':
        parser.error('Run on Linux or WSL')
    stock_pkg = args.stock.resolve()
    if not stock_pkg.is_file() or stock_pkg.stat().st_size != 99051117 or file_sha(stock_pkg) != STOCK_PKG_SHA:
        parser.error(f'Provide the complete original Nikon Z8 C3.11 Z_8_0311.bin (SHA-256 {STOCK_PKG_SHA})')
    qemu_root = args.qemu_root.resolve()
    if not (qemu_root / 'experiments/20261111/v36/from-stock/z8_slots_image.py').is_file():
        parser.error('Pass --qemu-root pointing to the companion z8-qemu repository (main branch)')

    core_clang, _ = find_compiler(args.core_clang, 18)
    live_clang, _ = find_compiler(args.live_clang, 21)
    llvm_bin = (args.llvm_bin or Path(live_clang).parent).resolve()
    os.environ['PATH'] = str(llvm_bin) + os.pathsep + os.environ['PATH']
    for tool in ('ld.lld', 'llvm-nm', 'llvm-objcopy', 'llvm-objdump'):
        if not shutil.which(tool):
            parser.error(f'Missing LLVM 21 tool: {tool}')

    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    deps = out / 'dependencies'
    packages = {k: extract_toolkit(k, None, deps) for k in DEPENDENCIES}

    # Stage 0 & Stage 1
    stage0_main, seed_manifest, stock_main = build_stage0(
        stock_pkg, qemu_root, out, core_clang, live_clang, packages, args.no_xm6,
    )
    stage1_main, stage1_build_json, stage1_ranges_json = build_stage1(
        stage0_main, seed_manifest, out, live_clang,
    )

    # Stage 2: PortraitPB
    print('=== [Stage 2] Applying PortraitPB ===', flush=True)
    s2 = out / 'stage2'
    s2.mkdir(exist_ok=True)
    s2_main = s2 / 'stage2.main'
    s2_meta = s2 / 'stage2-portrait.json'
    s1_sha = file_sha(stage1_main)
    subprocess.run([
        sys.executable, '-B', str(ASSETS / 'portrait_pb/patch_portrait_pb.py'),
        '--input', str(stage1_main), '--expected-sha256', s1_sha,
        '--build-manifest', str(stage1_build_json),
        '--output', str(s2_main), '--meta', str(s2_meta),
        '--version', 'v71r7-lut-preview-stage2-portraitpb',
    ], check=True)
    s2_sha = file_sha(s2_main)
    print(f'  [Stage 2 DONE] stage2.main SHA-256 = {s2_sha}', flush=True)

    # Stage 3: FontCJK-ZhUI
    final_main = out / 'main.main'
    if args.skip_fontzh:
        shutil.copy2(s2_main, final_main)
        print('=== [Stage 3] Skipped FontCJK-ZhUI (--skip-fontzh) ===', flush=True)
    else:
        font_path = args.font.resolve() if args.font else find_default_font()
        if not font_path or not font_path.is_file():
            raise RuntimeError(
                'No CJK TrueType font found for Stage 3 FontCJK-ZhUI. '
                'Pass --font /path/to/msyh.ttc (or NotoSansCJK-Regular.ttc), or use --skip-fontzh.'
            )
        print(f'=== [Stage 3] Building FontCJK-ZhUI from {font_path.name} ===', flush=True)
        s3 = out / 'stage3'
        s3_fonts = s3 / 'fonts'
        s3_fonts.mkdir(parents=True, exist_ok=True)
        font_zh = V71_WORK / 'font-zh'
        subprocess.run([
            sys.executable, '-B', str(font_zh / 'build_cjk_hif.py'),
            '--stock-main', str(stock_main), '--ttf', str(font_path), '--out-dir', str(s3_fonts),
        ], check=True)
        subprocess.run([
            sys.executable, '-B', str(font_zh / 'patch_v68.py'),
            '--base-main', str(s2_main), '--expected-base-sha256', s2_sha,
            '--build-manifest', str(stage1_ranges_json),
            '--cjk1', str(s3_fonts / 'cjk_ext_1.hif'),
            '--cjk2', str(s3_fonts / 'cjk_ext_2.hif'),
            '--c-src', str(font_zh / 'v68_patch_code.c'),
            '--version', 'v71r7-lut-preview',
            '--out-main', str(final_main), '--out-json', str(s3 / 'main.json'),
        ], check=True)
        subprocess.run([
            sys.executable, '-B', str(font_zh / 'audit_v68.py'),
            '--base-main', str(s2_main), '--expected-base-sha256', s2_sha,
            '--v68-main', str(final_main), '--v68-json', str(s3 / 'main.json'),
            '--cjk1', str(s3_fonts / 'cjk_ext_1.hif'),
            '--cjk2', str(s3_fonts / 'cjk_ext_2.hif'),
            '--out-json', str(s3 / 'stage3-fontzh-audit.json'),
        ], check=True)
        print(f'  [Stage 3 DONE] final main.main SHA-256 = {file_sha(final_main)}', flush=True)

    # Stage 4: Pack Z_8_0406.bin
    final_main_sha = file_sha(final_main)
    pkg_sha = None
    if not args.main_only:
        print('=== [Stage 4] Packing Z_8_0406.bin ===', flush=True)
        pkg_path = out / 'Z_8_0406.bin'
        pack_update_container(packages['rc2'], stock_pkg, final_main, pkg_path)
        pkg_sha = file_sha(pkg_path)
        print(f'  [Stage 4 DONE] Z_8_0406.bin SHA-256 = {pkg_sha}', flush=True)

    receipt = {
        'schema': 'v71r7.from-stock-reconstruction/v1',
        'stock_package_sha256': STOCK_PKG_SHA,
        'no_xm6': args.no_xm6,
        'skip_fontzh': args.skip_fontzh,
        'stage0_sha256': file_sha(stage0_main),
        'stage1_sha256': file_sha(stage1_main),
        'stage2_sha256': s2_sha,
        'main_sha256': final_main_sha,
        'package_sha256': pkg_sha,
        'exact_match_v71r7r7_main': final_main_sha == EXPECTED_R7_MAIN_SHA,
        'exact_match_v71r7r7_package': pkg_sha == EXPECTED_R7_PKG_SHA,
    }
    (out / 'reconstruction-v71r7.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps(receipt, indent=2), flush=True)


if __name__ == '__main__':
    main()
