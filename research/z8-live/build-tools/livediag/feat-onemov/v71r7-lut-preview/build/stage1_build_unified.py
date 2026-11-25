#!/usr/bin/env python3
"""Stage 1 — Compile & link unified V71 FilmSim 30fps All-in-One 40-unit EL1 payload
(21 src-fix37 scalar units [V71 Live Native-NV12 FilmSim Modes 0/1/2 + Future-Flip Guard
 + Batch fr_post_check + Tightened 120Hz trans[] + -24 Quiet-Park & diag_close Telemetry
 + V70 Silky30fps Cadence/Picker/CFR + Wave5 Direct-NV12/1-Deep Async/SWAR HEVC
 + V69 HangFix + #9 cfr_repeats + FilmSim hooks + RAW+JPEG fd=0 fix]
 + 18 FilmSim EL1 units [15 scalar + 3 NEON kernels: V71 LIVE_256_YC HDF + V4 17^3 Fuji LUT
 + Pre-Baked 512x256 Twisted-Torus Zf Grain + Simultaneous Z8_STYLE_FUJI_ZF + Post-Roll Defer]
 + 1 V71 NEON Direct NV12/I420 Scaler + Fused Grain/Copy/Preview kernel [filmsim_19-live_scale_neon.c.o]),
verify ELF sections & per-object FP/SIMD audit on win-lab WSL, and patch stage0-base.main
with all 51 Stage 1 hooks (Live / Compose / Menu Context / Review / Duplex Shutter / HangFix / FilmSim Photo)
-> stage1.main.
"""
from __future__ import annotations

import base64
import hashlib
import io
import json
import subprocess
import tarfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
V71 = HERE.parent
SRC_FIX37 = V71 / 'work/src-fix37'
NEON_SCALE = V71 / 'work/neon_scale'
FILMSIM = V71 / 'work/filmsim'
LUT_CORE = V71 / 'work/lut_core'
LUT_PREVIEW = V71 / 'work/lut_preview'
LUT_MENU = V71 / 'work/lut_menu'
FENESTRA_INC = Path('/Users/ycx/Nikon/fenestra-z8/include')
FINAL = V71 / 'final'
REMOTE_STAGE0_MAIN = '/home/ycx/fxfix/onemov/v71r7-lut-preview/stage0/out/stage0-base.main'
REMOTE_STAGE0_SEED_DIR = '/home/ycx/fxfix/onemov/v71r6-xm6-dsp/stage0/out'
REMOTE_ROOT = '/home/ycx/fxfix/onemov/v71r7-lut-preview/stage1'


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def pack_stage1_inputs() -> tuple[bytes, dict[str, str]]:
    files: dict[str, str] = {}
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode='w:gz') as tf:
        def add_tree(root: Path, prefix: str):
            for p in sorted(root.rglob('*')):
                if not p.is_file():
                    continue
                if any(part in ('__pycache__', 'build', 'out', 'objects') for part in p.relative_to(root).parts):
                    continue
                if p.name.startswith(('._', 'test_', 'run_')) or p.suffix in ('.o', '.log', '.orig'):
                    continue
                rel = prefix + '/' + str(p.relative_to(root))
                files[rel] = sha(p.read_bytes())
                tf.add(p, arcname=rel, recursive=False)

        add_tree(SRC_FIX37, 'src-fix37')
        add_tree(NEON_SCALE, 'neon_scale')
        add_tree(FILMSIM / 'src', 'filmsim/src')
        add_tree(LUT_CORE, 'lut_core')
        add_tree(LUT_PREVIEW, 'lut_preview')
        add_tree(LUT_MENU, 'lut_menu')
        for fname in ('build_filmsim_objs.sh', 'filmsim_section.ld.inc', 'patch_photo_v69.py'):
            p = FILMSIM / fname
            rel = 'filmsim/' + fname
            files[rel] = sha(p.read_bytes())
            tf.add(p, arcname=rel, recursive=False)
        for p in sorted(FENESTRA_INC.rglob('*.h')):
            rel = 'fenestra-include/' + str(p.relative_to(FENESTRA_INC))
            files[rel] = sha(p.read_bytes())
            tf.add(p, arcname=rel, recursive=False)
        for fname in ('patch_menu_fallback.py', 'patch_menu_context.py',
                      'patch_review_call.py', 'patch_shutter_calls.py',
                      'callsite-guards-r5.json'):
            p = HERE / fname
            rel = 'hooks/' + fname
            files[rel] = sha(p.read_bytes())
            tf.add(p, arcname=rel, recursive=False)
    return buf.getvalue(), files


def main() -> None:
    stage0_meta = json.loads((FINAL / 'stage0-base.json').read_text())
    stage0_sha = stage0_meta['sha256']
    assert len(stage0_meta.get('evf_pool6_patches', [])) == 2, 'Missing Bug #2 evf_pool6_patches in stage0-base.json'
    assert 'boot_panic03_cardslot' in stage0_meta, 'Missing V71.3 boot_panic03_cardslot in stage0-base.json'
    assert 'c64_imenu_ui' in stage0_meta, 'Missing V71.3 c64_imenu_ui in stage0-base.json'
    assert 'nfp_v050' in stage0_meta, 'Missing V71.3 nfp_v050 in stage0-base.json'
    assert 'xm6_elf0' in stage0_meta, 'Missing V71.6 xm6_elf0 in stage0-base.json'
    assert sha((FINAL / 'stage0-base.main').read_bytes()) == stage0_sha

    raw_tar, files = pack_stage1_inputs()
    b64_tar = base64.b64encode(raw_tar).decode('ascii')

    remote_script = f'''set -euo pipefail
ROOT={REMOTE_ROOT!r}
STAGE0_DST={REMOTE_STAGE0_MAIN!r}
STAGE0_DIR="$(dirname "$STAGE0_DST")"
STAGE0_SEED={REMOTE_STAGE0_SEED_DIR!r}
if [ ! -f "$STAGE0_DST" ] && [ -f "$STAGE0_SEED/stage0-base.main" ]; then
  mkdir -p "$STAGE0_DIR"
  cp -f "$STAGE0_SEED/stage0-base.main" "$STAGE0_DIR/stage0-base.main"
  [ -f "$STAGE0_SEED/stage0-base.json" ] && cp -f "$STAGE0_SEED/stage0-base.json" "$STAGE0_DIR/stage0-base.json"
  [ -f "$STAGE0_SEED/pb_only_v37.main" ] && cp -f "$STAGE0_SEED/pb_only_v37.main" "$STAGE0_DIR/pb_only_v37.main"
fi
test -f "$STAGE0_DST"
python3 -c "import hashlib, pathlib; s=hashlib.sha256(pathlib.Path('$STAGE0_DST').read_bytes()).hexdigest(); assert s == '{stage0_sha}', (s, '{stage0_sha}'); print('Verified WSL Stage 0 SHA-256:', s)"

rm -rf "$ROOT"
mkdir -p "$ROOT/source" "$ROOT/out/objects"

python3 - <<'PY'
import base64, hashlib, io, json, tarfile
from pathlib import Path
root = Path({REMOTE_ROOT!r})
src = root / 'source'
data = base64.b64decode({b64_tar!r})
with tarfile.open(fileobj=io.BytesIO(data), mode='r:gz') as tf:
    for m in tf.getmembers():
        assert not m.name.startswith('/') and '..' not in Path(m.name).parts and m.isfile()
    tf.extractall(src)
expected = json.loads({json.dumps(files)!r})
for rel, h in expected.items():
    got = hashlib.sha256((src / rel).read_bytes()).hexdigest()
    assert got == h, f"Upload hash mismatch: {{rel}} {{got}} != {{h}}"
print(f"Verified {{len(expected)}} uploaded Stage 1 files", flush=True)
PY

chmod +x "$ROOT/source/filmsim/build_filmsim_objs.sh"

# 1. Build the 15 FilmSim EL1 objects (13 scalar -mgeneral-regs-only + 2 selective-NEON kernels)
FILMSIM_SRC="$ROOT/source/filmsim/src" \
SRC_FIX37_DIR="$ROOT/source/src-fix37" \
LUT_CORE_DIR="$ROOT/source/lut_core" \
LUT_PREVIEW_DIR="$ROOT/source/lut_preview" \
LUT_MENU_DIR="$ROOT/source/lut_menu" \
FENESTRA_INC_DIR="$ROOT/source/fenestra-include" \
OUT_DIR="$ROOT/out/objects" \
CC=clang-21 \
  "$ROOT/source/filmsim/build_filmsim_objs.sh"

# 2. Build the 19th .filmsim object: V71.1 NEON Direct NV12 / I420 Scaler (incl. 5:4 EVF Downscaler), Copy & Preview kernel
cp -f "$ROOT/source/neon_scale/live_scale_neon.h" "$ROOT/source/src-fix37/capture/live_scale_neon.h"
clang-21 --target=aarch64-none-elf -O2 -std=c11 \
  -ffreestanding -fno-builtin -fno-stack-protector \
  -fno-unwind-tables -fno-asynchronous-unwind-tables \
  -ffunction-sections -fdata-sections -mstrict-align -mno-outline-atomics \
  -Wall -Wextra -Werror \
  -I"$ROOT/source/neon_scale" \
  -I"$ROOT/source/src-fix37/capture" \
  -c "$ROOT/source/neon_scale/live_scale_neon.c" \
  -o "$ROOT/out/objects/filmsim_19-live_scale_neon.c.o"
echo "Built filmsim_19-live_scale_neon.c.o cleanly (selective-NEON -O2)"

# 3. Build all 21 src-fix37 scalar units (-DLIVE_FILMSIM_POOL_HOOKS=1 -Os -mgeneral-regs-only -mstrict-align -Wall -Wextra -Werror),
#    link unified 40-unit payload.elf, verify ELF/FP/SIMD invariants, and patch stage0-base.main -> stage1.main
/home/ycx/.local/share/nikon-z8-qemu/venv/bin/python -B - <<'PY'
import hashlib, json, re, shutil, struct, subprocess, sys
from pathlib import Path

ROOT = Path({REMOTE_ROOT!r})
SRC = ROOT / 'source'
OUT = ROOT / 'out'
OBJ = OUT / 'objects'
STAGE0_MAIN = Path({REMOTE_STAGE0_MAIN!r})
STAGE0_SHA = {stage0_sha!r}
BASE = 0x40160000
ID = 0x401fe060 - BASE

sha = lambda b: hashlib.sha256(b).hexdigest()

clang = str(Path(shutil.which('clang-21')).resolve())
version = subprocess.check_output([clang, '--version'], text=True).splitlines()[0]
assert 'clang version 21.' in version, version

src_fix37 = SRC / 'src-fix37'
filmsim_src = SRC / 'filmsim/src'

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
    o = OBJ / ('live_' + rel.replace('/', '_') + '.o')
    live_objs.append(o)
    flags = [
        '--target=aarch64-none-elf', '-Os', '-ffreestanding', '-fno-builtin',
        '-fno-stack-protector', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables',
        '-ffunction-sections', '-fdata-sections',
        '-mgeneral-regs-only', '-mstrict-align',
        '-DLA_WIDTH=1024', '-DLA_HEIGHT=680', '-DLA_RING_CAPACITY=96', '-DLA_SAMPLE_INTERVAL_US=25000',
        '-DLIVE_BITRATE=8000000', '-DLIVE_IDR_US=500000', '-DLIVE_ENCODE_SPEED=9',
        '-DLIVE_FILMSIM_POOL_HOOKS=1',
        '-Wall', '-Wextra', '-Werror',
        '-I', str(SRC / 'neon_scale'),
        '-I', str(src_fix37),
        '-I', str(src_fix37 / 'ui/core/include'),
        '-I', str(src_fix37 / 'codec/freestanding'),
        '-I', str(filmsim_src / 'runtime'),
        '-I', str(filmsim_src / 'include'),
    ]
    subprocess.run([clang, *flags, '-c', str(src_fix37 / rel), '-o', str(o)], check=True)

filmsim_objs = sorted(OBJ.glob('filmsim_*.o'))
assert len(live_objs) == 21 and len(filmsim_objs) == 16, (len(live_objs), len(filmsim_objs))

# 3b. Build V71.7 Bank 1 (lut_core 7 scalar + 2 NEON; lut_preview 3 scalar = 12 units -> v71r7_b1_*.o)
lut_core_src = SRC / 'lut_core'
lut_preview_src = SRC / 'lut_preview'
lut_menu_src = SRC / 'lut_menu'

b1_common = [
    '--target=aarch64-none-elf', '-std=c11', '-ffreestanding', '-fno-builtin',
    '-fno-stack-protector', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables',
    '-ffunction-sections', '-fdata-sections', '-mstrict-align', '-mno-outline-atomics',
    '-Wall', '-Wextra', '-Werror',
    '-I', str(lut_core_src),
    '-I', str(lut_preview_src),
    '-I', str(src_fix37 / 'codec/freestanding'),
    '-I', str(filmsim_src / 'include'),
    '-I', str(filmsim_src / 'xm6'),
    '-I', str(filmsim_src / 'photo'),
    '-I', str(filmsim_src / 'pipeline'),
]

b1_objs = []
for rel in ('cube.c', 'cube_frame.c', 'layer0_color.c', 'neon_sentinel.c',
            'photo_flight.c', 'smp.c', 'lut_runtime.c'):
    o = OBJ / f'v71r7_b1_{{rel}}.o'
    b1_objs.append(o)
    subprocess.run([clang, '-Os', '-mgeneral-regs-only', *b1_common, '-c', str(lut_core_src / rel), '-o', str(o)], check=True)

for rel in ('cube_frame_neon.c', 'layer0_color_neon.c'):
    o = OBJ / f'v71r7_b1_{{rel}}.o'
    b1_objs.append(o)
    subprocess.run([clang, '-O2', *b1_common, '-c', str(lut_core_src / rel), '-o', str(o)], check=True)

for rel in ('photo_workspace.c', 'layer0_provider.c'):
    o = OBJ / f'v71r7_b1_{{rel}}.o'
    b1_objs.append(o)
    subprocess.run([clang, '-Oz', '-mgeneral-regs-only', *b1_common, '-c', str(lut_preview_src / rel), '-o', str(o)], check=True)

o_hooks = OBJ / 'v71r7_b1_lut_hooks.S.o'
b1_objs.append(o_hooks)
subprocess.run([clang, '--target=aarch64-none-elf', '-c', str(lut_preview_src / 'lut_hooks.S'), '-o', str(o_hooks)], check=True)

# 3c. Build V71.7 Bank 2 (lut_menu 5 scalar units -> v71r7_b2_*.o)
b2_flags = [
    '--target=aarch64-none-elf', '-Oz', '-std=c11', '-ffreestanding', '-fno-builtin',
    '-fno-pic', '-fno-stack-protector', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables',
    '-ffunction-sections', '-fdata-sections', '-mgeneral-regs-only', '-mstrict-align',
    '-mno-outline-atomics', '-Wall', '-Wextra', '-Werror',
    '-I', str(lut_core_src), '-I', str(lut_menu_src),
]
b2_objs = []
for rel in ('photo_catalog.c', 'photo_journal.c', 'photo_ui.c', 'photo_worker.c', 'photo_menu_ro_tables.c'):
    o = OBJ / f'v71r7_b2_{{rel}}.o'
    b2_objs.append(o)
    subprocess.run([clang, *b2_flags, '-c', str(lut_menu_src / rel), '-o', str(o)], check=True)

assert len(b1_objs) == 12 and len(b2_objs) == 5, (len(b1_objs), len(b2_objs))
print(f"Compiled {{len(live_objs)}} src-fix37 + {{len(filmsim_objs)}} .filmsim + {{len(b1_objs)}} Bank 1 + {{len(b2_objs)}} Bank 2 objects (54 total) cleanly", flush=True)

# 4. Build unified payload.ld from seed-manifest.json + filmsim_section.ld.inc
seed_manifest = json.loads(Path('/home/ycx/v37build/v36/seed-manifest.json').read_text())
seed_ranges = {{int(r['address'], 16): r for r in seed_manifest['ranges']}}
seed_syms = seed_manifest['symbols']

def sym_int(v):
    return int(v, 16) if isinstance(v, str) else int(v)

def seed_target(site):
    w = struct.unpack('<I', bytes.fromhex(seed_ranges[site]['expected']))[0]
    assert (w >> 26) in (5, 37), (hex(site), hex(w))
    return site + ((((w & 0x3ffffff) ^ 0x2000000) - 0x2000000) * 4)

exports = [
    'fx_memset', 'fx_native_menu_build', 'fx_native_menu_link',
    'fx_native_display_attach_global', 'fx_native_display_detach',
    'fx_native_display_is_current', 'fx_native_display_present',
    'memcpy', 'memmove', 'memset',
]
defs = {{k: sym_int(seed_syms[k]) for k in exports}}
defs['memcmp'] = sym_int(seed_syms['fx_memcmp'])
defs.update(
    live_old_menu_page=seed_target(0x403db694),
    live_old_menu_text=seed_target(0x4032e130),
    live_old_system_dispatch=seed_target(0x40d60ec8),
    live_old_movie_press=seed_target(0x40fdc9c8),
    live_irq_resume=0x40e9083c,
    live_old_writer=seed_target(0x40ea3638),
    live_closed_resume=0x408b7c20,
    live_start_resume=0x40c26808,
    live_stop_resume=0x40c2681c,
    live_hold_resume=0x40c2682c,
    live_old_pool=seed_target(0x40e8b328),
    live_w5_bringup_resume=0x402f2d5c,
    live_stock_shutter_prepare=0x40f9cccc,
    live_stock_shutter_fire=0x40f9cd54,
)

filmsim_inc_body = (SRC / 'filmsim/filmsim_section.ld.inc').read_text()

ld_body = """
ENTRY(live_ui_system_dispatch)
SECTIONS {{
  .live_shutter 0x4019f020 : {{ *(EXCLUDE_FILE(*filmsim_*.o *v71r7_b1_*.o *v71r7_b2_*.o) .live_shutter) }}
  ASSERT(. <= 0x4019ff40, "Live shutter bank exhausted")

  . = 0x401e2000;
  .text : {{ *(EXCLUDE_FILE(*filmsim_*.o *v71r7_b1_*.o *v71r7_b2_*.o) .text*) }}
  .rodata : {{ *(EXCLUDE_FILE(*filmsim_*.o *v71r7_b1_*.o *v71r7_b2_*.o) .rodata*) }}
  live_primary_end = .;
  ASSERT(live_primary_end <= 0x401fcd00, "live_primary collides with PortraitPB cave 0x401FCD00")

  .live_extra 0x401fd218 : {{ *(EXCLUDE_FILE(*filmsim_*.o *v71r7_b1_*.o *v71r7_b2_*.o) .live_extra) }}
  ASSERT(. <= 0x401fe000, "Live extra cave exhausted")

  .v71r7_lut_menu 0x40981000 : {{
    *v71r7_b2_*.o(.text*)
    *(.v71r7_lut_menu .v71r7_lut_menu.*)
    *(.filmsim.pw)
    . = ALIGN(16);
    *v71r7_b2_*.o(.rodata*)
  }}
  ASSERT(. <= 0x40990000, "Bank 2 v71r7_lut_menu 0x40981000..0x40990000 exhausted")

__FILMSIM__

  .v71r7_lut_core 0x409db000 : {{
    *v71r7_b1_*.o(.text*)
    *filmsim_20-z8_xm6_bridge.c.o(.text*)
    *(.v71r7_lut_core .v71r7_lut_core.*)
    . = ALIGN(16);
    *v71r7_b1_*.o(.rodata*)
    *filmsim_20-z8_xm6_bridge.c.o(.rodata*)
  }}
  ASSERT(. <= 0x409ec000, "Bank 1 v71r7_lut_core 0x409DB000..0x409EC000 exhausted")

  .data : {{ *(.data*) }}
  .bss : {{ *(.bss*) *(COMMON) }}
  /DISCARD/ : {{ *(.comment) *(.note*) *(.eh_frame*) }}
}}
""".replace('__FILMSIM__', filmsim_inc_body)
ld_text = '\\n'.join(f'{{k}} = {{v:#x}};' for k, v in defs.items()) + ld_body
ld_path = OUT / 'payload.ld'
ld_path.write_text(ld_text)

elf_path = OUT / 'payload.elf'
subprocess.run(['ld.lld', '-T', str(ld_path), *map(str, live_objs), *map(str, filmsim_objs),
                *map(str, b1_objs), *map(str, b2_objs), '-o', str(elf_path)], check=True)

# 5. Strict ELF section split + per-object FP/SIMD + symbol audit (54 units: 49 scalar + 5 NEON kernels)
undef = [l for l in subprocess.check_output(['llvm-nm', '-u', str(elf_path)], text=True).splitlines() if l.strip()]
assert not undef, f"Undefined symbols in unified payload.elf: {{undef}}"

fp_re = re.compile(r'\\b(?:fadd|fsub|fmul|fdiv|fmadd|fmsub|fnmadd|fnmsub|fneg|fabs|fsqrt|fcmp|fcmpe|fccmp|fcsel|fcvt[a-z]*|scvtf|ucvtf|frint[a-z]*|fmax[a-z]*|fmin[a-z]*|frecp[a-z]*|frsq[a-z]*)\\b')
simd_re = re.compile(r'\\b(?:v\\d+\\.|[bhsdq]\\d+\\b|fmov|fadd|fsub|fmul|fdiv|scvtf|ucvtf|fcvtz|fcvt|fcmp|fmadd|fmsub|dup\\s|ushll|sshll|uaddw|usubw|uqadd|uqsub|shrn|ld[1-4]\\s*\\{{|st[1-4]\\s*\\{{|movi\\s+v)')
neon_allowed = {{
    'filmsim_02-z8_hdf_kernels.c.o',
    'filmsim_07-z8_zf_grain_kernels.c.o',
    'filmsim_19-live_scale_neon.c.o',
    'v71r7_b1_cube_frame_neon.c.o',
    'v71r7_b1_layer0_color_neon.c.o',
}}

per_obj = {{}}
total_fp = 0
non_kernel_simd = 0
all_objs = sorted(OBJ.glob('*.o'))
assert len(all_objs) == 54, len(all_objs)
for o in all_objs:
    dis = subprocess.check_output(['llvm-objdump', '-d', str(o)], text=True)
    fp_cnt = simd_cnt = insns = 0
    for line in dis.splitlines():
        m_ins = re.match(r'^\\s+[0-9a-f]+:\\s+([0-9a-f]{{8}})\\s+(.*)$', line)
        if not m_ins:
            continue
        word_hex, rest = m_ins.groups()
        if o.name == 'filmsim_18-photo_hooks.S.o' and int(word_hex, 16) in (0x40eaafc4, 0x40eab76c, 0x40ea44d4):
            continue
        insns += 1
        if fp_re.search(rest):
            fp_cnt += 1
        if simd_re.search(rest):
            simd_cnt += 1
    total_fp += fp_cnt
    if o.name not in neon_allowed:
        non_kernel_simd += simd_cnt
    per_obj[o.name] = dict(insns=insns, fp_insns=fp_cnt, simd_insns=simd_cnt, neon_allowed=o.name in neon_allowed)

assert total_fp == 0, f"FP instructions found: {{[k for k, v in per_obj.items() if v['fp_insns']]}}"
assert non_kernel_simd == 0, f"Non-kernel SIMD found: {{[(k, v['simd_insns']) for k, v in per_obj.items() if not v['neon_allowed'] and v['simd_insns']]}}"

# Parse ELF directly to extract all 4 RX regions strictly without holes
elf_bytes = elf_path.read_bytes()
assert elf_bytes[:6] == b'\\x7fELF\\x02\\x01' and struct.unpack_from('<H', elf_bytes, 18)[0] == 183
shoff = struct.unpack_from('<Q', elf_bytes, 40)[0]
ents, num, shstrndx = struct.unpack_from('<HHH', elf_bytes, 58)
hdrs = [struct.unpack_from('<IIQQQQIIQQ', elf_bytes, shoff + i * 64) for i in range(num)]
strtab_hdr = hdrs[shstrndx]
shstrs = elf_bytes[strtab_hdr[4]:strtab_hdr[4] + strtab_hdr[5]]
sec_names = [shstrs[h[0]:].split(b'\\0', 1)[0].decode() for h in hdrs]

symbols = {{}}
regions = {{'shutter': [], 'primary': [], 'extra': [], 'lut_menu': [], 'filmsim': [], 'lut_core': []}}
bounds = {{
    'shutter':  (0x4019f020, 0x4019ff40),
    'primary':  (0x401e2000, 0x401fcd00),
    'extra':    (0x401fd218, 0x401fe000),
    'lut_menu': (0x40981000, 0x40990000),
    'filmsim':  (0x409b0000, 0x409d8000),
    'lut_core': (0x409db000, 0x409ec000),
}}
sorted_banks = sorted(bounds.items(), key=lambda kv: kv[1][0])
for i in range(len(sorted_banks) - 1):
    assert sorted_banks[i][1][1] <= sorted_banks[i + 1][1][0], (sorted_banks[i], sorted_banks[i + 1])

for name, h in zip(sec_names, hdrs):
    typ, flags, addr, off, size = h[1:6]
    if typ == 2:
        st = hdrs[h[6]]
        strings = elf_bytes[st[4]:st[4] + st[5]]
        for at in range(off, off + size, 24):
            nm, info, other, idx, value, sz = struct.unpack_from('<IBBHQQ', elf_bytes, at)
            if idx != 0:
                sname = strings[nm:].split(b'\\0', 1)[0].decode()
                if sname:
                    symbols[sname] = value
    if not (flags & 2) or not size:
        continue
    assert not (flags & 1) and typ != 8, f"Writable/NOBITS alloc section forbidden: {{name}} size={{size}}"
    matched = None
    for rname, (lo, hi) in bounds.items():
        if lo <= addr and addr + size <= hi:
            matched = rname
            break
    assert matched is not None, f"Section {{name}} @ {{addr:#x}}+{{size}} outside allowed RX banks"
    if matched == 'shutter': assert name == '.live_shutter', name
    if matched == 'extra': assert name == '.live_extra', name
    if matched == 'lut_menu': assert name == '.v71r7_lut_menu', name
    if matched == 'filmsim': assert name == '.filmsim', name
    if matched == 'lut_core': assert name == '.v71r7_lut_core', name
    regions[matched].append((addr, name, elf_bytes[off:off + size]))

blobs = {{}}
sections_info = []
for rname, (lo, hi) in bounds.items():
    rs = sorted(regions[rname])
    assert rs and rs[0][0] == lo, (rname, rs)
    data = bytearray()
    cur_end = lo
    for addr, name, payload in rs:
        assert addr >= cur_end, (rname, name, hex(addr), hex(cur_end))
        data.extend(bytes(addr - cur_end))
        data.extend(payload)
        cur_end = addr + len(payload)
        sections_info.append(dict(bank=rname, name=name, address=hex(addr), size=len(payload), sha256=sha(payload)))
    assert cur_end <= hi, (rname, hex(cur_end), hex(hi))
    blobs[rname] = bytes(data)

# Verify HangFix -> FilmSim pool coordination & diag_close calls actually exist in linked .live_extra disassembly!
dis_extra = subprocess.check_output(['llvm-objdump', '-d', '-j', '.live_extra', str(elf_path)], text=True)
assert '<filmsim_runtime_pool_suspend>' in dis_extra, 'Missing call to filmsim_runtime_pool_suspend in .live_extra!'
assert '<filmsim_runtime_pool_resume>' in dis_extra, 'Missing call to filmsim_runtime_pool_resume in .live_extra!'
assert '<live_encoder_diag_close>' in dis_extra, 'Missing call to live_encoder_diag_close in .live_extra!'

# Verify live_ui.c & live_stream.c & runtime.c call V71 FilmSim / encoder hooks in .text / .filmsim / .live_extra
dis_text = subprocess.check_output(['llvm-objdump', '-d', '-j', '.text', str(elf_path)], text=True)
dis_filmsim = subprocess.check_output(['llvm-objdump', '-d', '-j', '.filmsim', str(elf_path)], text=True)
dis_b1 = subprocess.check_output(['llvm-objdump', '-d', '-j', '.v71r7_lut_core', str(elf_path)], text=True)
dis_b2 = subprocess.check_output(['llvm-objdump', '-d', '-j', '.v71r7_lut_menu', str(elf_path)], text=True)
dis_all = dis_text + '\\n' + dis_extra + '\\n' + dis_filmsim + '\\n' + dis_b1 + '\\n' + dis_b2

for req_call in ('filmsim_runtime_ui_poll', 'filmsim_runtime_on_stop', 'filmsim_runtime_on_card_change',
                 'filmsim_runtime_live_mode', 'filmsim_runtime_live_begin_submit_nv12',
                 'filmsim_runtime_live_commit_submit_nv12', 'filmsim_runtime_live_acquire_grain_tile',
                 'filmsim_runtime_live_release', 'live_encoder_is_parked',
                 'v71r7_photo_menu_boot_once', 'v71r7_photo_menu_lookup_text',
                 'v71r7_photo_menu_on_card_change'):
    assert f'<{{req_call}}>' in dis_text, f'Missing call to {{req_call}} in .text!'

for req_v71r7 in ('v71r7_layer0_pool_suspend', 'v71r7_layer0_pool_resume', 'v71r7_layer0_set_wysiwyg_ops',
                  'v71r7_layer0_lifecycle_stop', 'v71r7_lut_apply_still_nv16', 'v71r7_lut_apply_nv12',
                  'v71r7_lut_bind_xm6', 'v71r7_neon_sentinel_is_allowed', 'v71r7_neon_sentinel_probe_once'):
    assert f'<{{req_v71r7}}>' in dis_all, f'Missing V71.7 call to {{req_v71r7}} in linked ELF!'

for b1_sym in ('v71r7_layer0_enqueue_hook', 'v71r7_layer0_display_hook', 'v71r7_layer0_vsync_hook',
               'v71r7_power_sync_hook', 'v71r7_layer0_pool_suspend', 'v71r7_layer0_pool_resume',
               'v71r7_lut_prepare_from_buffer', 'v71r7_lut_stream_begin', 'v71r7_lut_stream_feed',
               'v71r7_lut_stream_commit', 'v71r7_lut_stream_abort',
               'v71r7_lut_apply_still_nv16', 'v71r7_lut_apply_nv12',
               'v71r7_neon_sentinel_is_allowed', 'v71r7_flight_record', 'v71r7_smp_run_stripes'):
    assert b1_sym in symbols and 0x409db000 <= symbols[b1_sym] < 0x409ec000, (b1_sym, hex(symbols.get(b1_sym, 0)))

for b2_sym in ('v71r7_photo_menu_boot_once', 'v71r7_photo_menu_lookup_text',
               'v71r7_photo_menu_on_card_change', 'v71r7_photo_parent_desc',
               'v71r7_lut_child_vector', 'v71r7_lut_child_desc'):
    assert b2_sym in symbols and 0x40981000 <= symbols[b2_sym] < 0x40990000, (b2_sym, hex(symbols.get(b2_sym, 0)))

# Verify V71 + V70 FilmSim/NEON/Wave5/cadence symbols are linked in their exact expected RX banks!
for v71_sym in (
    'filmsim_runtime_live_mode',
    'filmsim_runtime_live_begin_submit_nv12',
    'filmsim_runtime_live_commit_submit_nv12',
    'filmsim_runtime_live_acquire_nv12',
    'filmsim_runtime_live_acquire_grain_tile',
    'filmsim_runtime_live_release',
    'live_scale_1024_to_1280_nv12_neon',
    'live_scale_1024_to_1280_nv12_neon_grain',
    'live_scale_1024_to_1280_i420_neon',
    'live_downscale_1280_to_1024_nv12_neon',
    'live_copy_nv12_neon',
    'live_copy_nv12_neon_grain',
    'live_preview_nv12_to_i420_1024x680',
    'z8_hdf_live_nv12_1024',
    'z8_zf_grain_live_bake_tile',
    'z8_zf_grain_live_apply_tile_y',
    'z8_filmsim_apply_live_nv12',
    'live_runtime_post_roll_active',
    'live_encoder_is_parked',
    'live_encoder_diag_close',
    'live_encoder_nv12_begin',
    'live_encoder_submit_slot',
    'live_encoder_poll_au',
    'live_encoder_flush',
    'live_encoder_last_valid_slot',
    'live_encoder_slot_ptrs',
    'live_encoder_diag_timings',
    'la_native_observe_flip',
    'la_stream_flip_obs',
):
    assert v71_sym in symbols, f"Missing V71 symbol {{v71_sym}}"
    va = symbols[v71_sym]
    if v71_sym.startswith(('live_scale_', 'live_downscale_', 'live_copy_', 'live_preview_', 'z8_hdf_', 'z8_film_', 'z8_zf_', 'z8_filmsim_', 'filmsim_runtime_')):
        assert 0x409b0000 <= va < 0x409d8000, f"FilmSim/NEON symbol {{v71_sym}} @ {{va:#x}} not in .filmsim!"
    else:
        assert (0x401e2000 <= va < 0x401fcd00) or (0x409b0000 <= va < 0x409d8000), (v71_sym, hex(va))

# Verify V71 .text/.filmsim actually calls all live FilmSim, NEON scaler+grain, Wave5 async & cadence symbols
for req_v71 in (
    'filmsim_runtime_live_mode', 'filmsim_runtime_live_begin_submit_nv12',
    'filmsim_runtime_live_commit_submit_nv12', 'filmsim_runtime_live_acquire_nv12',
    'filmsim_runtime_live_acquire_grain_tile', 'filmsim_runtime_live_release',
    'live_scale_1024_to_1280_nv12_neon', 'live_scale_1024_to_1280_nv12_neon_grain',
    'live_downscale_1280_to_1024_nv12_neon',
    'live_copy_nv12_neon', 'live_copy_nv12_neon_grain', 'live_preview_nv12_to_i420_1024x680',
    'z8_hdf_live_nv12_1024',
    'z8_zf_grain_live_bake_tile', 'live_runtime_post_roll_active',
    'live_encoder_is_parked', 'live_encoder_diag_close',
    'live_encoder_nv12_begin', 'live_encoder_submit_slot', 'live_encoder_poll_au',
    'live_encoder_flush', 'live_encoder_diag_timings', 'la_native_observe_flip',
):
    assert f'<{{req_v71}}>' in dis_all, f'Missing call to V71 symbol {{req_v71}} in linked ELF!'

# 6. Patch stage0-base.main -> stage1.main
stage0_bytes = STAGE0_MAIN.read_bytes()
assert sha(stage0_bytes) == STAGE0_SHA
img = bytearray(stage0_bytes)

def enc_branch(at, target, link=False):
    delta = target - at
    assert delta % 4 == 0 and -(1 << 27) <= delta < (1 << 27), (hex(at), hex(target))
    return struct.pack('<I', (0x94000000 if link else 0x14000000) | ((delta // 4) & 0x3ffffff))

hook_records = []

# 6a. Write all 4 unified ELF RX banks (strict zero-guard!)
for rname, (lo, hi) in bounds.items():
    blob = blobs[rname]
    off = lo - BASE
    assert not any(img[off:off + (hi - lo)]), f"Bank {{rname}} ({{lo:#x}}..{{hi:#x}}) not zero in stage0-base.main!"
    img[off:off + len(blob)] = blob

# 6b. Live 16 stock sites (exact relive_v37.py SITES)
LIVE_SITES = [
    (0x403db694, 'live_ui_menu_page', False),
    (0x4032e130, 'live_ui_menu_text', False),
    (0x40d60ec8, 'live_ui_system_dispatch', False),
    (0x40fdc9c8, 'live_press_entry', False),
    (0x40e90838, 'live_irq_entry', False),
    (0x40c26804, 'live_start_entry', False),
    (0x40c26818, 'live_stop_entry', False),
    (0x40c26828, 'live_hold_entry', False),
    (0x40e8b328, 'live_pool_wrap', True),
    (0x408b7c1c, 'live_closed_entry', False),
    (0x406cd6a0, 'live_finalize_wrap', True),
    (0x406cd75c, 'live_event_wrap', True),
    (0x40ea3638, 'live_writer_wrap', True),
    (0x406cd468, 'live_open_wrap', True),
    (0x402f2d58, 'live_w5_bringup_entry', False),
    (0x40f9cb64, 'live_shutter_entry', True),
]
extra_expected = {{0x402f2d58: 'ff0302d1', 0x40f9cb64: '08014039'}}
for at, sname, is_call in LIVE_SITES:
    exp_hex = extra_expected.get(at) or seed_ranges[at]['expected']
    got_hex = img[at - BASE:at - BASE + 4].hex()
    assert got_hex == exp_hex, f"Live site {{at:#x}} ({{sname}}) expected {{exp_hex}}, got {{got_hex}}"
    rep = enc_branch(at, symbols[sname], link=is_call)
    img[at - BASE:at - BASE + 4] = rep
    hook_records.append(dict(group='live_v37', address=hex(at), size=4, instruction='BL' if is_call else 'B',
                             symbol=sname, target=hex(symbols[sname]), expected=exp_hex, replacement=rep.hex()))

# 6c. 4 compose_base53 / compose53 Live overlays
COMPOSE_LIVE = [
    (0x40f6cff0, '37bf0094', 'live_shutter_prepare_entry', True, 'passive shutter prepare'),
    (0x40f6e174, 'f8ba0094', 'live_shutter_fire_entry', True, 'passive shutter fire'),
    (0x40e30e3c, '3c0f0014', 'live_shutter_dispatch', False, 'SFX owner dispatch'),
]
for at, exp_hex, sname, is_call, purpose in COMPOSE_LIVE:
    got_hex = img[at - BASE:at - BASE + 4].hex()
    assert got_hex == exp_hex, f"Compose site {{at:#x}} ({{sname}}) expected {{exp_hex}}, got {{got_hex}}"
    rep = enc_branch(at, symbols[sname], link=is_call)
    img[at - BASE:at - BASE + 4] = rep
    hook_records.append(dict(group='compose_live', address=hex(at), size=4, instruction='BL' if is_call else 'B',
                             symbol=sname, target=hex(symbols[sname]), expected=exp_hex, replacement=rep.hex(), purpose=purpose))

evf_ptr_at = 0x401def00
evf_exp = struct.pack('<Q', 0x403efed8)
assert bytes(img[evf_ptr_at - BASE:evf_ptr_at - BASE + 8]) == evf_exp, '0x401def00 mismatch'
evf_target = symbols['live_ui_evf_submit']
assert 0x401e2000 <= evf_target < 0x401fcd00 or 0x401fd218 <= evf_target < 0x401fe000
evf_rep = struct.pack('<Q', evf_target)
img[evf_ptr_at - BASE:evf_ptr_at - BASE + 8] = evf_rep
hook_records.append(dict(group='compose_live', address=hex(evf_ptr_at), size=8, instruction='PTR64',
                         symbol='live_ui_evf_submit', target=hex(evf_target),
                         expected=evf_exp.hex(), replacement=evf_rep.hex(), purpose='Passive EVF submit observation'))

pre_menu_bytes = bytes(img)
pre_menu_sha = sha(pre_menu_bytes)

# 6d. Menu context hook (0x401CD410 + 0x401FCF40..0x401FCFF0)
sys.path.insert(0, str(SRC / 'hooks'))
import patch_menu_context, patch_review_call, patch_shutter_calls

live_primary_bytes = len(blobs['primary'])
build_compat = dict(
    live_payload_bytes=live_primary_bytes,
    live_free=0x401fd000 - (0x401e2000 + live_primary_bytes),
)
menu_bytes, menu_facts = patch_menu_context.patch(pre_menu_bytes, pre_menu_sha, build_compat)
hook_records.append(dict(group='menu_context', address=menu_facts['hook']['address'], size=4, instruction='BL',
                         symbol='menu_context_helper', target='0x401fcf40',
                         expected=menu_facts['hook']['before'], replacement=menu_facts['hook']['after']))

# 6e. Image Review slot1c deferral hook (0x40407f84 -> BL live_review_pop)
review_bytes, review_facts = patch_review_call.patch(menu_bytes, sha(menu_bytes), symbols['live_review_pop'])
hook_records.append(dict(group='review_deferral', address=review_facts['address'], size=4, instruction='BL',
                         symbol='live_review_pop', target=review_facts['target'],
                         expected=review_facts['before'], replacement=review_facts['after']))

# 6f. 11 Full-Duplex Shutter hooks (callsite-guards-r5.json)
duplex_guards = json.loads((SRC / 'hooks/callsite-guards-r5.json').read_text())
duplex_bytes, duplex_facts = patch_shutter_calls.patch(review_bytes, sha(review_bytes), symbols, duplex_guards)
for df in duplex_facts:
    hook_records.append(dict(group='duplex_shutter', address=df['site'], size=4, instruction=df['branch'],
                             symbol=df['symbol'], target=df['target'],
                             expected=struct.pack('<I', int(df['before'], 16)).hex(),
                             replacement=struct.pack('<I', int(df['after'], 16)).hex()))

# 6g. HangFix + PB Bounce Guard hooks from hangfix_hooks.json (exact original-byte guarded!)
hangfix_spec = json.loads((src_fix37 / 'hangfix_hooks.json').read_text())
hangfix_img = bytearray(duplex_bytes)
hangfix_facts = []
for h in hangfix_spec['hooks']:
    at = int(h['address'], 16)
    exp_hex = h['original_hex']
    sname = h['target_symbol']
    target = symbols[sname]
    assert (0x401e2000 <= target < 0x401fcd00) or (0x401fd218 <= target < 0x401fe000), (sname, hex(target))
    if h['kind'] == 'PTR64':
        sz = 8
        got_hex = hangfix_img[at - BASE:at - BASE + 8].hex()
        assert got_hex == exp_hex, f"HangFix PTR64 site {{at:#x}} ({{h['name']}}) expected {{exp_hex}}, got {{got_hex}}"
        rep = struct.pack('<Q', target)
        hangfix_img[at - BASE:at - BASE + 8] = rep
    else:
        sz = 4
        got_hex = hangfix_img[at - BASE:at - BASE + 4].hex()
        assert got_hex == exp_hex, f"HangFix site {{at:#x}} ({{h['name']}}) expected {{exp_hex}}, got {{got_hex}}"
        is_call = (h['kind'] == 'BL')
        rep = enc_branch(at, target, link=is_call)
        hangfix_img[at - BASE:at - BASE + 4] = rep
    rec = dict(group='hangfix', name=h['name'], address=hex(at), size=sz, instruction=h['kind'],
               symbol=sname, target=hex(target), expected=exp_hex, replacement=rep.hex(), layer=h['layer'])
    hangfix_facts.append(rec)
    hook_records.append(rec)
hangfix_bytes = bytes(hangfix_img)

# 6h. 14 FilmSim photo hooks via patch_photo_v69.py
sys.path.insert(0, str(SRC / 'filmsim'))
import patch_photo_v69
photo_bytes, photo_patches = patch_photo_v69.apply(
    hangfix_bytes, symbols, expected_base_sha256=sha(hangfix_bytes), require_bank=True
)
assert len(photo_patches) == 14
for pp in photo_patches:
    hook_records.append(dict(group='filmsim_photo', **pp))

# 6h2. 6 verified-clean V71.7 Layer 0 & Power Sync stock ROM hooks
v71r7_img = bytearray(photo_bytes)
V71R7_LAYER0_HOOKS = [
    (0x40e9a540, '66f5f897', 'v71r7_layer0_enqueue_hook', True, 'Layer 0 enqueue hook'),
    (0x40cd862c, 'd779d797', 'v71r7_layer0_display_hook', True, 'Layer 0 display swap hook'),
    (0x40cd72a4, 'e7080094', 'v71r7_layer0_vsync_hook',   True, 'Layer 0 LCD VSync hook'),
    (0x40cd72d4, 'db080094', 'v71r7_layer0_vsync_hook',   True, 'Layer 0 EVF VSync hook'),
    (0x40d67328, 'dbe6ff97', 'v71r7_power_sync_hook',     True, 'POWER_CANCEL_SYNC (w0=11) hook'),
    (0x40d67340, 'd5e6ff97', 'v71r7_power_sync_hook',     True, 'POWER_OFF_SYNC (w0=10) hook'),
]
v71r7_layer0_facts = []
for at, exp_hex, sname, is_call, purpose in V71R7_LAYER0_HOOKS:
    got_hex = v71r7_img[at - BASE:at - BASE + 4].hex()
    assert got_hex == exp_hex, f"V71.7 Layer0 site {{at:#x}} ({{sname}}) expected {{exp_hex}}, got {{got_hex}}"
    rep = enc_branch(at, symbols[sname], link=is_call)
    v71r7_img[at - BASE:at - BASE + 4] = rep
    rec = dict(group='v71r7_layer0_power', address=hex(at), size=4, instruction='BL' if is_call else 'B',
               symbol=sname, target=hex(symbols[sname]), expected=exp_hex, replacement=rep.hex(), purpose=purpose)
    v71r7_layer0_facts.append(rec)
    hook_records.append(rec)

# 6h3. 2 V71.7 Photo Shooting Menu RAM init patches (0xA028F5670 & 0xA02917D00)
sys.path.insert(0, str(SRC / 'lut_menu'))
import gen_photo_menu_tables
ram_init_patches = gen_photo_menu_tables.build_ram_init_patches(
    stage0_bytes,
    parent_desc_va=symbols['v71r7_photo_parent_desc'],
    lut_vector_va=symbols['v71r7_lut_child_vector'],
    lut_desc_va=symbols['v71r7_lut_child_desc'],
)
assert len(ram_init_patches) == 2
for rp in ram_init_patches:
    foff = int(rp['file_offset'], 16)
    sz = int(rp['bytes'])
    old_b = bytes.fromhex(rp['old_hex'])
    new_b = bytes.fromhex(rp['new_hex'])
    assert len(old_b) == sz and len(new_b) == sz
    assert bytes(v71r7_img[foff:foff + sz]) == old_b, f"RAM init patch {{rp['name']}} preimage mismatch at {{foff:#x}}"
    v71r7_img[foff:foff + sz] = new_b

assert len(hook_records) == 62, f"Expected 62 Stage 1 ROM hooks, got {{len(hook_records)}}"

# 6i. Stamp Stage 1 Bootguard Build ID at 0x401fe060
final_img = v71r7_img
struct.pack_into('<I', final_img, ID, 0)
build_id = struct.unpack('<I', hashlib.sha256(final_img).digest()[:4])[0] | 0x01000000
struct.pack_into('<I', final_img, ID, build_id)
stage1_bytes = bytes(final_img)

# 7. Exhaustive Stage 1 Self-Audit
# 7a. Exact 0 unexplained byte check vs stage0-base.main
approved = set()
for rname, (lo, hi) in bounds.items():
    approved.update(range(lo - BASE, lo - BASE + len(blobs[rname])))
approved.update(range(0x401fcf40 - BASE, 0x401fcf40 - BASE + len(patch_menu_context.HELPER_BYTES)))
approved.update(range(0x401fe060 - BASE, 0x401fe064 - BASE))
for hr in hook_records:
    a = int(hr['address'], 16)
    approved.update(range(a - BASE, a - BASE + hr['size']))
for rp in ram_init_patches:
    foff = int(rp['file_offset'], 16)
    approved.update(range(foff, foff + int(rp['bytes'])))

changed = 0
unknown = []
for off in range(0, len(stage0_bytes), 65536):
    end = min(off + 65536, len(stage0_bytes))
    if stage0_bytes[off:end] != stage1_bytes[off:end]:
        for idx in range(off, end):
            if stage0_bytes[idx] != stage1_bytes[idx]:
                changed += 1
                if idx not in approved:
                    unknown.append(BASE + idx)
assert not unknown, f"Unexplained bytes in stage1.main: {{[hex(x) for x in unknown[:20]]}}"

# 7b. Verify reserved caves for Stage 2 (PortraitPB) and Stage 3 (FontCJK) are 100% ZERO!
reserved_zero = [
    ('portrait_pb_cave', 0x401fcd00, 0x401fcf40),
    ('font_zh_overlay',  0x401fef00, 0x401fff80),
    ('cjk_ext_1',        0x408c6000, 0x40951000),
    ('cjk_ext_2',        0x40b8a000, 0x40c00000),
]
for zname, zs, ze in reserved_zero:
    assert not any(stage1_bytes[zs - BASE:ze - BASE]), f"Reserved cave {{zname}} ({{zs:#x}}..{{ze:#x}}) NOT ZERO!"

# 7c. Verify Stage 0 PB bank & Fenestra Core pb_output_* & cardslot/portrait hooks & EVF Pool #6 patches untouched
assert stage1_bytes[0x40196000 - BASE:0x4019f020 - BASE] == stage0_bytes[0x40196000 - BASE:0x4019f020 - BASE]
assert stage1_bytes[0x401fe20c - BASE:0x401fe210 - BASE].hex() == '61b4d454'
assert stage1_bytes[0x401fe280 - BASE:0x401fe284 - BASE].hex() == '5cd62b14'
assert stage1_bytes[0x401fe300 - BASE:0x401feeb8 - BASE] == stage0_bytes[0x401fe300 - BASE:0x401feeb8 - BASE]
assert stage1_bytes[0x40b66000 - BASE:0x40b70000 - BASE] == stage0_bytes[0x40b66000 - BASE:0x40b70000 - BASE]
assert stage1_bytes[0x40af0000 - BASE:0x40afa000 - BASE] == stage0_bytes[0x40af0000 - BASE:0x40afa000 - BASE]
assert stage1_bytes[0x40b12000 - BASE:0x40b1b000 - BASE] == stage0_bytes[0x40b12000 - BASE:0x40b1b000 - BASE]
assert struct.unpack_from('<I', stage1_bytes, 0x40cf4620 - BASE)[0] == 0x5284000d
assert struct.unpack_from('<I', stage1_bytes, 0x40cf4630 - BASE)[0] == 0x72a0038d

# 7d. Verify 0x4032e130 decodes to exact live_ui_menu_text inside [0x401e2000, 0x401fcd00)
w_mt = struct.unpack_from('<I', stage1_bytes, 0x4032e130 - BASE)[0]
assert (w_mt >> 26) == 5
mt_imm = ((w_mt & 0x3ffffff) ^ 0x2000000) - 0x2000000
mt_va = 0x4032e130 + mt_imm * 4
assert mt_va == symbols['live_ui_menu_text'] and 0x401e2000 <= mt_va < 0x401fcd00

# 8. Write outputs
(OUT / 'stage1.main').write_bytes(stage1_bytes)
stage1_ranges = dict(
    schema='v71.stage1-ranges/v2',
    stage0_sha256=STAGE0_SHA,
    stage1_sha256=sha(stage1_bytes),
    build_id=hex(build_id),
    sections=sections_info,
    hooks=hook_records,
    ram_init_patches=ram_init_patches,
    symbols={{k: v for k, v in sorted(symbols.items()) if k.startswith(('live_', 'lp_', 'la_', 'pb_', 'z8_', 'filmsim_', 'v71r7_', 'photo_'))}},
)
(OUT / 'stage1-ranges.json').write_text(json.dumps(stage1_ranges, indent=2) + '\\n')

stage1_build = dict(
    schema='v71.stage1-build/v2',
    compiler=version,
    stage0_sha256=STAGE0_SHA,
    pre_menu_sha256=pre_menu_sha,
    menu_patch_sha256=sha(menu_bytes),
    review_patch_sha256=sha(review_bytes),
    duplex_patch_sha256=sha(duplex_bytes),
    hangfix_patch_sha256=sha(hangfix_bytes),
    photo_patch_sha256=sha(photo_bytes),
    stage1_sha256=sha(stage1_bytes),
    build_id=hex(build_id),
    bytes=len(stage1_bytes),
    changed_bytes_vs_stage0=changed,
    unexplained_bytes=0,
    # Compatibility keys required by patch_portrait_pb.py & audit_portrait_pb.py:
    live_payload_bytes=len(blobs['primary']),
    live_free=0x401fd000 - (0x401e2000 + len(blobs['primary'])),
    live_primary_start='0x401e2000',
    live_primary_end_exclusive=hex(0x401e2000 + len(blobs['primary'])),
    live_free_before_portrait_pb=0x401fcd00 - (0x401e2000 + len(blobs['primary'])),
    shutter_bytes=len(blobs['shutter']),
    shutter_free=(0x4019ff40 - 0x4019f020) - len(blobs['shutter']),
    live_extra_bytes=len(blobs['extra']),
    live_extra_free=(0x401fe000 - 0x401fd218) - len(blobs['extra']),
    filmsim_bytes=len(blobs['filmsim']),
    filmsim_free=(0x409d8000 - 0x409b0000) - len(blobs['filmsim']),
    filmsim_start='0x409b0000',
    filmsim_end_exclusive=hex(0x409b0000 + len(blobs['filmsim'])),
    lut_core_bytes=len(blobs['lut_core']),
    lut_core_free=(0x409ec000 - 0x409db000) - len(blobs['lut_core']),
    lut_core_start='0x409db000',
    lut_core_end_exclusive=hex(0x409db000 + len(blobs['lut_core'])),
    lut_menu_bytes=len(blobs['lut_menu']),
    lut_menu_free=(0x40990000 - 0x40981000) - len(blobs['lut_menu']),
    lut_menu_start='0x40981000',
    lut_menu_end_exclusive=hex(0x40981000 + len(blobs['lut_menu'])),
    playback_payload_bytes=36288,
    playback_free=608,
    live_ui_menu_text=hex(symbols['live_ui_menu_text']),
    live_review_pop=hex(symbols['live_review_pop']),
    key_symbols={{k: hex(symbols[k]) for k in (
        'live_ui_menu_text', 'live_review_pop', 'live_layout_switch_wrap',
        'live_subpool_carve_wrap', 'live_pb_arbiter_exit_wrap',
        'live_movs_alloc_wrap', 'live_prores_bringup_entry',
        'filmsim_runtime_pool_suspend', 'filmsim_runtime_pool_resume',
        'filmsim_runtime_ui_poll', 'z8_np_engine19_job2_stub', 'z8_np_engine19_stub',
        'z8_np_hook_resize', 'z8_np_hook_thumbnail', 'z8_np_hook_cleanup',
        'live_scale_1024_to_1280_nv12_neon', 'live_scale_1024_to_1280_nv12_neon_grain',
        'live_scale_1024_to_1280_i420_neon', 'live_downscale_1280_to_1024_nv12_neon',
        'live_copy_nv12_neon', 'live_copy_nv12_neon_grain',
        'live_preview_nv12_to_i420_1024x680',
        'filmsim_runtime_live_mode', 'filmsim_runtime_live_begin_submit_nv12',
        'filmsim_runtime_live_commit_submit_nv12', 'filmsim_runtime_live_acquire_nv12',
        'filmsim_runtime_live_acquire_grain_tile', 'filmsim_runtime_live_release',
        'z8_hdf_live_nv12_1024',
        'z8_zf_grain_live_bake_tile', 'z8_zf_grain_live_apply_tile_y', 'z8_filmsim_apply_live_nv12',
        'live_runtime_post_roll_active', 'live_encoder_is_parked', 'live_encoder_diag_close',
        'live_encoder_nv12_begin', 'live_encoder_submit_slot', 'live_encoder_poll_au',
        'live_encoder_flush', 'live_encoder_last_valid_slot', 'live_encoder_slot_ptrs',
        'live_encoder_diag_timings', 'la_native_observe_flip', 'la_stream_flip_obs',
        'v71r7_layer0_enqueue_hook', 'v71r7_layer0_display_hook', 'v71r7_layer0_vsync_hook',
        'v71r7_power_sync_hook', 'v71r7_layer0_pool_suspend', 'v71r7_layer0_pool_resume',
        'v71r7_lut_prepare_from_buffer', 'v71r7_lut_stream_begin', 'v71r7_lut_stream_feed',
        'v71r7_lut_stream_commit', 'v71r7_lut_stream_abort',
        'v71r7_lut_apply_still_nv16', 'v71r7_lut_apply_nv12',
        'v71r7_neon_sentinel_is_allowed', 'v71r7_neon_sentinel_probe_once',
        'v71r7_photo_menu_boot_once', 'v71r7_photo_menu_lookup_text',
        'v71r7_photo_menu_on_card_change', 'v71r7_photo_parent_desc',
        'v71r7_lut_child_vector', 'v71r7_lut_child_desc',
    )}},
    elf_simd_audit=dict(
        units_compiled=len(per_obj),
        scalar_units=len(per_obj) - len(neon_allowed),
        neon_kernel_units=len(neon_allowed),
        total_fp_instructions=total_fp,
        non_kernel_simd_instructions=non_kernel_simd,
        neon_kernel_simd_instructions={{k: per_obj[k]['simd_insns'] for k in sorted(neon_allowed)}},
        writable_sections=[],
        undefined_symbols=[],
        hangfix_filmsim_pool_calls_verified=True,
        v71_filmsim_neon_and_async_calls_verified=True,
        v71r7_lut_preview_and_menu_calls_verified=True,
    ),
    hook_counts={{
        'live_v37': 16,
        'compose_live': 4,
        'menu_context': 1,
        'review_deferral': 1,
        'duplex_shutter': len(duplex_facts),
        'hangfix': len(hangfix_facts),
        'filmsim_photo': len(photo_patches),
        'v71r7_layer0_power': len(v71r7_layer0_facts),
        'v71r7_photo_menu_ram_init': len(ram_init_patches),
        'total_stage1_hooks': len(hook_records),
    }},
    reserved_caves_zero=[dict(name=n, start=hex(s), end_exclusive=hex(e), bytes=e-s) for n, s, e in reserved_zero],
    menu_context=menu_facts,
    review_hook=review_facts,
    duplex_hooks=duplex_facts,
    hangfix_hooks=hangfix_facts,
    filmsim_photo_hooks=photo_patches,
    v71r7_layer0_hooks=v71r7_layer0_facts,
    v71r7_ram_init_patches=ram_init_patches,
)
(OUT / 'stage1-build.json').write_text(json.dumps(stage1_build, indent=2) + '\\n')
(OUT / 'stage1-simd-audit.json').write_text(json.dumps(dict(per_object=per_obj, summary=stage1_build['elf_simd_audit']), indent=2) + '\\n')

print('STAGE1_OK ' + json.dumps({{
    'stage1_sha256': stage1_build['stage1_sha256'],
    'build_id': stage1_build['build_id'],
    'shutter': f"{{stage1_build['shutter_bytes']}} B (free {{stage1_build['shutter_free']}} B)",
    'live_primary': f"{{stage1_build['live_payload_bytes']}} B -> {{stage1_build['live_primary_end_exclusive']}} (free before PortraitPB {{stage1_build['live_free_before_portrait_pb']}} B)",
    'live_extra': f"{{stage1_build['live_extra_bytes']}} B (free {{stage1_build['live_extra_free']}} B)",
    'filmsim': f"{{stage1_build['filmsim_bytes']}} B -> {{stage1_build['filmsim_end_exclusive']}} (free {{stage1_build['filmsim_free']}} B)",
    'lut_core_bank1': f"{{stage1_build['lut_core_bytes']}} B -> {{stage1_build['lut_core_end_exclusive']}} (free {{stage1_build['lut_core_free']}} B)",
    'lut_menu_bank2': f"{{stage1_build['lut_menu_bytes']}} B -> {{stage1_build['lut_menu_end_exclusive']}} (free {{stage1_build['lut_menu_free']}} B)",
    'live_ui_menu_text': stage1_build['live_ui_menu_text'],
    'total_stage1_hooks': stage1_build['hook_counts']['total_stage1_hooks'],
    'ram_init_patches': len(ram_init_patches),
    'unexplained_bytes': 0,
}}))
PY
'''

    res = subprocess.run(
        ['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=8', 'win-lab', 'wsl -e bash -s'],
        input=remote_script, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    )
    print(res.stdout, end='')
    if res.returncode != 0:
        raise SystemExit(res.returncode)

    # Fetch Stage 1 artifacts back to Mac cleanly
    fetch_script = f'''python3 - <<'PY'
import base64, io, sys, tarfile
from pathlib import Path
out = Path({REMOTE_ROOT!r}) / 'out'
buf = io.BytesIO()
files = {{
    'final/stage1.main': out / 'stage1.main',
    'final/payload.elf': out / 'payload.elf',
    'final/stage1-ranges.json': out / 'stage1-ranges.json',
    'final/stage1-build.json': out / 'stage1-build.json',
    'final/stage1-simd-audit.json': out / 'stage1-simd-audit.json',
    'build/payload.ld': out / 'payload.ld',
}}
with tarfile.open(fileobj=buf, mode='w:gz') as tf:
    for arc, p in files.items():
        tf.add(p, arcname=arc, recursive=False)
sys.stdout.write(base64.b64encode(buf.getvalue()).decode('ascii'))
PY
'''
    encoded = subprocess.check_output(
        ['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=8', 'win-lab', 'wsl -e bash -s'],
        input=fetch_script.encode(),
    )
    with tarfile.open(fileobj=io.BytesIO(base64.b64decode(encoded, validate=True)), mode='r:gz') as tf:
        for m in tf.getmembers():
            assert m.isfile() and not m.name.startswith('/') and '..' not in Path(m.name).parts
        tf.extractall(V71)

    meta = json.loads((FINAL / 'stage1-build.json').read_text())
    assert sha((FINAL / 'stage1.main').read_bytes()) == meta['stage1_sha256']
    print("Fetched Stage 1 artifacts cleanly:", FINAL / 'stage1.main', meta['stage1_sha256'])


if __name__ == '__main__':
    main()
