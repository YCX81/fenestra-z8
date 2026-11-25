#!/usr/bin/env python3
"""Stage 0 — Rebuild V69 pb-fix37 (35,824 B) and compose clean V71.1 base image
(stock C3.11 + V69 pb-fix37 + Fenestra Core with relocated pb_output_* + NFP +
USB/pool/bootguard + cardslot/portrait52/portrait53 + crop/aspect +
Bug #2 EVF MOVS Pool #6 slot-size 0x00151800 -> 0x001C2000 patch at
0x40cf4620 / 0x40cf4630, with ALL non-PB payload banks 100% zero).
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
PB_SRC = V71 / 'work/pb-fix37'
FINAL = V71 / 'final'
REMOTE_ROOT = '/home/ycx/fxfix/onemov/v71r7-lut-preview/stage0'


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def pack_pb_sources() -> tuple[bytes, dict[str, str]]:
    files: dict[str, str] = {}
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode='w:gz') as tf:
        for p in sorted(PB_SRC.rglob('*')):
            if p.is_file() and '__pycache__' not in p.parts and not p.name.startswith('._'):
                rel = 'pb-fix37/' + str(p.relative_to(PB_SRC))
                files[rel] = sha(p.read_bytes())
                tf.add(p, arcname=rel, recursive=False)
        for rel_name in ('relive_pb53.py', 'patch_boot_panic03_cardslot.py', 'patch_c64_imenu_ui.py', 'patch_nfp_v050.py', 'patch_xm6_elf0_stage0.py'):
            p = HERE / rel_name
            files[rel_name] = sha(p.read_bytes())
            tf.add(p, arcname=rel_name, recursive=False)
        for rel_name in ('cave.bin', 'manifest.json'):
            p = HERE / 'cardslot_v71r4' / rel_name
            arc = 'cardslot_v71r4/' + rel_name
            files[arc] = sha(p.read_bytes())
            tf.add(p, arcname=arc, recursive=False)
        for rel_name in ('v050_bundle.bin', 'manifest.json'):
            p = HERE / 'nfp_v050' / rel_name
            arc = 'nfp_v050/' + rel_name
            files[arc] = sha(p.read_bytes())
            tf.add(p, arcname=arc, recursive=False)
        for rel_name in ('text_z8fx_0x418c4580.bin', 'rodata_idm_0x0003bc40.bin', 'coexist_manifest.json'):
            p = HERE / 'xm6_elf0/coexist_nikondma' / rel_name
            arc = 'xm6_elf0/coexist_nikondma/' + rel_name
            files[arc] = sha(p.read_bytes())
            tf.add(p, arcname=arc, recursive=False)
    return buf.getvalue(), files


def main() -> None:
    raw_tar, files = pack_pb_sources()
    b64_tar = base64.b64encode(raw_tar).decode('ascii')

    remote_script = f'''set -euo pipefail
ROOT={REMOTE_ROOT!r}
rm -rf "$ROOT"
mkdir -p "$ROOT/source" "$ROOT/out"

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
print(f"Verified {{len(expected)}} uploaded Stage 0 files", flush=True)
PY

/home/ycx/.local/share/nikon-z8-qemu/venv/bin/python -B - <<'PY'
import hashlib, json, os, re, shutil, struct, subprocess, sys
from pathlib import Path

ROOT = Path({REMOTE_ROOT!r})
SRC = ROOT / 'source'
OUT = ROOT / 'out'
H = Path('/home/ycx')
V36 = H / 'v37build/v36'
TOOLS_V37 = H / 'v51src/research/z8-live/build-tools/livediag/feat-release/tools-v37'
RELEASE = TOOLS_V37.parent
ARCHIVE = RELEASE.parents[2]
V53 = H / 'fxfix/v53-root'
V52 = H / 'fxfix/v52-root'
BASE = 0x40160000
ID = 0x401fe060 - BASE

sha = lambda b: hashlib.sha256(b).hexdigest()

seed = V36 / 'seed.main'
seed_manifest = V36 / 'seed-manifest.json'
assert sha(seed.read_bytes()) == '50f2a5b22217710c6f35faf1e4639ef675f2738184525be0190f36bcc42cb36b'

clang = str(Path(shutil.which('clang-21')).resolve())
version = subprocess.check_output([clang, '--version'], text=True).splitlines()[0]
assert 'clang version 21.' in version, version
env = dict(os.environ, V36_REAL_CLANG=clang,
           PATH=os.pathsep.join([str(TOOLS_V37 / 'compiler'), str(Path(clang).parent), os.environ['PATH']]))

# 1. Build V69 pb-fix37 against seed.main with exact V68 -Oz wrapper
pb_out = OUT / 'pb'
subprocess.run([
    sys.executable, '-B', str(SRC / 'relive_pb53.py'),
    str(SRC / 'pb-fix37'), str(seed), str(seed_manifest), str(pb_out),
    '--defs=' + json.dumps({{'fx_glyph': '0x401f0000', 'fx_memmove': '0x401f0100'}})
], env=env, check=True)

pb_bin = (pb_out / 'payload.bin').read_bytes()
pb_elf = pb_out / 'payload.elf'
assert 0 < len(pb_bin) <= (0x4019f020 - 0x40196000), f"pb-fix37 overflow: {{len(pb_bin)}} B"

# Verify 0 FP/NEON and 0 writable sections in pb.elf
dis = subprocess.check_output(['llvm-objdump', '-d', str(pb_elf)], text=True)
simd_re = re.compile(r'\\b(?:v\\d+\\.|[bhsdq]\\d+\\b|fmov|fadd|fsub|fmul|fdiv|scvtf|ucvtf|fcvtz|fcvt|fcmp|fmadd|fmsub)\\b')
bad_insns = []
for l in dis.splitlines():
    m_ins = re.match(r'^\\s*[0-9a-f]+:\\s+[0-9a-f]{{8}}\\s+(.*)$', l)
    if m_ins and simd_re.search(m_ins.group(1)):
        bad_insns.append(l.strip())
assert not bad_insns, f"FP/NEON in pb.elf: {{bad_insns[:5]}}"
shdr = subprocess.check_output(['llvm-readelf', '-SW', str(pb_elf)], text=True)
for line in shdr.splitlines():
    m = re.match(r'^\\s*\\[\\s*\\d+\\]\\s+(\\S+)\\s+(\\S+)\\s+[0-9a-f]+\\s+[0-9a-f]+\\s+([0-9a-f]+)\\s+[0-9a-f]+\\s+(\\S+)', line)
    if m and 'A' in m.group(4) and 'W' in m.group(4):
        assert int(m.group(3), 16) == 0, f"Writable section in pb.elf: {{line}}"

# 2. Apply usb_default_on -> gui_pool_fix -> bootguard_patch on pb-only image
pb_main = pb_out / 'seed.main'
assert pb_main.exists()
(OUT / 'usb').mkdir()
usb = OUT / 'usb/usb.main'
subprocess.run([sys.executable, '-B', str(ARCHIVE / 'build-tools/usb_default_on.py'), str(pb_main), str(usb)], check=True)
import importlib.util
spec = importlib.util.spec_from_file_location('v36_chain', RELEASE / 'tools-v36/chain.py')
chain = importlib.util.module_from_spec(spec)
spec.loader.exec_module(chain)
usb_man = chain.append_changes(pb_main.read_bytes(), usb.read_bytes(),
                               json.loads((pb_main.parent / 'changed-ranges.json').read_text()), 'USB default on')
(usb.parent / 'changed-ranges.json').write_text(json.dumps(usb_man, indent=2) + '\\n')
(OUT / 'pool').mkdir()
pool = OUT / 'pool/pool.main'
subprocess.run([sys.executable, '-B', str(ARCHIVE / 'build-tools/gui_pool_fix.py'), str(usb), str(pool)], check=True)
pb_v37 = OUT / 'pb_only_v37.main'
subprocess.run([sys.executable, '-B', str(RELEASE / 'tools-v36/bootguard_patch.py'), str(pool), str(pb_v37)], check=True)

# 3. Splice runs(v51d/v37.main -> pb_only_v37.main) onto v51d/q/main.main (exact compose_base53.py mechanics)
def runs(a, b, step=4096):
    i = 0
    while i < len(a):
        if a[i:i+step] == b[i:i+step]:
            i += step
            continue
        end = min(len(a), i + step)
        while i < end:
            if a[i] == b[i]:
                i += 1
                continue
            first = i
            while i < len(a) and a[i] != b[i]:
                i += 1
            yield first, i

old = (H / 'v37build/v51d/v37.main').read_bytes()
base = (H / 'fxfix/v51d/q/main.main').read_bytes()
assert sha(old) == '736aa971ba99a0aef3e5d155fd9db3f7c028adb09f9576761407502d2ac36e70'
assert sha(base) == 'aa6e94e0042fb445c666de73c0f6b9e7e569010b7993a3ac3da0979b174a9433'

new = bytearray(pb_v37.read_bytes())
new[ID:ID+4] = old[ID:ID+4]
image = bytearray(base)
changes = list(runs(old, new))
for start, end in changes:
    assert image[start:end] == old[start:end], f'Independent v51 overlay conflict at {{BASE+start:#x}}'
    image[start:end] = new[start:end]

# 4. Relocate Fenestra Core pb_output_* adapters to V69 pb-fix37 symbols
sys.path.insert(0, str(V52))
from core_output_patch import apply as apply_evf
pb_symbols = json.loads((pb_out / 'changed-ranges.json').read_text())['symbols']
image, evf_changes = apply_evf(image, pb_symbols)

# 5. Apply portrait52 + portrait53 + crop_fix + aspect_fix (exact compose_base53 + compose53 overlays)
sys.path.insert(0, str(V52 / 'portrait-overlay'))
from apply_portrait52_final import apply as apply_portrait52
image, portrait52_changes = apply_portrait52(bytes(image), base)

v52g_ref = (V52 / 'v52g/q/main.main').read_bytes()
assert sha(v52g_ref) == 'ed1e8496fccde443412e527a252de5029a55c749053b9b046da42bfc454aa613'
sys.path.insert(0, str(V53 / 'portrait'))
from portrait53 import apply as apply_portrait53
image, portrait53_changes = apply_portrait53(image, v52g_ref)
image = bytearray(image)

def branch(source, target):
    delta = target - source
    assert delta % 4 == 0 and -(1 << 27) <= delta < (1 << 27)
    return struct.pack('<I', 0x14000000 | ((delta // 4) & 0x3ffffff))

crop_aspect_overlays = []
for name, address, size, hook, expected in [
    ('crop_fix', 0x4019ff40, 128, 0x40cf7d08, bytes.fromhex('692a0079')),
    ('aspect_fix', 0x4019ffc0, 64, 0x407138d0, branch(0x407138d0, 0x409708f0)),
]:
    payload = (V53 / 'native-rec/crop-fix' / f'{{name}}.bin').read_bytes()
    assert len(payload) == size
    at = address - BASE
    assert image[at:at+size] == bytes(size), f'{{name}} cave not zero'
    image[at:at+size] = payload
    hat = hook - BASE
    assert image[hat:hat+4] == expected, f'{{name}} hook mismatch at {{hook:#x}}'
    image[hat:hat+4] = branch(hook, address)
    crop_aspect_overlays.append(dict(name=name, address=hex(address), size=size, hook=hex(hook)))

# 5b. Bug #2 Fix: MOVS Pool #6 EVF 3:2 slot-size patch (0x00151800 -> 0x001C2000 = 1280x960x1.5)
#     Stock C3.11 sizes Pool #6 slots to 1280x720x1.5 (0x151800), overflowing +0x3C000 on EVF 3:2 (1280x848 = 0x18D800).
evf_pool6_patches = []
for addr, old_word, new_word, old_asm, new_asm in [
    (0x40cf4620, 0x5283000d, 0x5284000d, 'mov w13, #0x1800', 'mov w13, #0x2000'),
    (0x40cf4630, 0x72a002ad, 0x72a0038d, 'movk w13, #0x15, lsl #16', 'movk w13, #0x1c, lsl #16'),
]:
    off = addr - BASE
    got_word = struct.unpack_from('<I', image, off)[0]
    assert got_word == old_word, f'EVF Pool #6 pre-patch mismatch at {{addr:#x}}: got {{got_word:#010x}}, expected {{old_word:#010x}}'
    struct.pack_into('<I', image, off, new_word)
    evf_pool6_patches.append(dict(
        address=hex(addr),
        size=4,
        old=struct.pack('<I', old_word).hex(),
        new=struct.pack('<I', new_word).hex(),
        old_word=f'0x{{old_word:08x}}',
        new_word=f'0x{{new_word:08x}}',
        old_asm=old_asm,
        new_asm=new_asm,
        slot_size_before='0x00151800',
        slot_size_after='0x001c2000',
    ))

# 5c. V71.3 Fix: Boot PANIC 03 (identity PAR_EL1 + CPU0 affinity) + Dual-Card cardslot.c SMP & unaligned hook_read
sys.path.insert(0, str(SRC))
import patch_boot_panic03_cardslot
boot_panic03_cardslot_meta = patch_boot_panic03_cardslot.apply(image, cardslot_dir=SRC / 'cardslot_v71r4')

# 5d. V71.3 Fix: C64 i-menu, Fn+Dial & Viewfinder OSD EL1 UI (0x251D, 12-tile, dial strip 95 cells, LV_INDICATOR, state2, LABEL_CODE icons)
import patch_c64_imenu_ui
c64_imenu_ui_meta = patch_c64_imenu_ui.apply(image)

# 5e. V71.5 Upstream Merge: NFP v0.5.1 (25fefa1, 4 feature groups relocated < 0x40B50000 + Q3_STASH -> 0xA0FF52000 + framing-fixes)
import patch_nfp_v050
nfp_v050_meta = patch_nfp_v050.apply(image, bundle_dir=SRC / 'nfp_v050')

# 5f. V71.6/V71.7 EXPEED 7 CEVA-XM6 Core 0 Coexist NikonDMA Overlay (Slot 39/40 FilmGrain + Slot 41 Ricoh HDF v2)
import patch_xm6_elf0_stage0
xm6_elf0_meta = patch_xm6_elf0_stage0.apply(image, payload_dir=SRC / 'xm6_elf0/coexist_nikondma')
patch_xm6_elf0_stage0.verify(image, xm6_elf0_meta)

# 6. Stamp Stage 0 Bootguard Build ID
struct.pack_into('<I', image, ID, 0)
build_id = struct.unpack('<I', hashlib.sha256(image).digest()[:4])[0] | 0x01000000
struct.pack_into('<I', image, ID, build_id)

# 7. Strict Stage 0 verification
assert image[0x401fd000-BASE:0x401fd218-BASE] == v52g_ref[0x401fd000-BASE:0x401fd218-BASE]
assert image[0x40196000-BASE:0x40196000-BASE+len(pb_bin)] == pb_bin
assert not any(image[0x40196000-BASE+len(pb_bin):0x4019f020-BASE])
assert struct.unpack_from('<I', image, 0x40cf4620-BASE)[0] == 0x5284000d, '0x40cf4620 post-patch mismatch'
assert struct.unpack_from('<I', image, 0x40cf4630-BASE)[0] == 0x72a0038d, '0x40cf4630 post-patch mismatch'

zero_banks = [
    ('shutter', 0x4019f020, 0x4019ff40),
    ('live_primary', 0x401e2000, 0x401fcd00),
    ('portrait_pb', 0x401fcd00, 0x401fcf40),
    ('menu_context_helper', 0x401fcf40, 0x401fd000),
    ('live_extra', 0x401fd218, 0x401fe000),
    ('font_overlay', 0x401fef00, 0x401fff80),
    ('cjk_ext_1', 0x408c6000, 0x40951000),
    ('filmsim', 0x409b0000, 0x409d8000),
    ('cjk_ext_2', 0x40b8a000, 0x40c00000),
]
for bname, bs, be in zero_banks:
    assert not any(image[bs-BASE:be-BASE]), f'Bank {{bname}} ({{bs:#x}}..{{be:#x}}) is NOT zero!'

assert image[0x401fe20c-BASE:0x401fe210-BASE].hex() == '61b4d454', '0x401FE20C mismatch'
assert image[0x401fe280-BASE:0x401fe284-BASE].hex() == '5cd62b14', '0x401FE280 mismatch'

# Verify V68 match on 0x401fe20c / 0x401fe280 and V71.3 cardslot_v71r4 cave (0x401fe300..0x401feeb8, keeping 0x401fef00 font_overlay zero)
v68 = (H / 'fxfix/onemov/v68-duplex30fps-888131/final/main.main').read_bytes()
assert image[0x401fe20c-BASE:0x401fe210-BASE] == v68[0x401fe20c-BASE:0x401fe210-BASE]
assert image[0x401fe280-BASE:0x401fe284-BASE] == v68[0x401fe280-BASE:0x401fe284-BASE]
expected_cs = (SRC / 'cardslot_v71r4/cave.bin').read_bytes()
assert len(expected_cs) == 3000 and image[0x401fe300-BASE:0x401fe300-BASE+len(expected_cs)] == expected_cs
assert not any(image[0x401fe300-BASE+len(expected_cs):0x401fef00-BASE])

stage0_main = OUT / 'stage0-base.main'
stage0_main.write_bytes(image)
meta = dict(
    schema='v71.stage0-base/v2',
    compiler=version,
    sha256=sha(bytes(image)),
    build_id=hex(build_id),
    bytes=len(image),
    pb_payload_bytes=len(pb_bin),
    pb_payload_free=(0x4019f020 - 0x40196000) - len(pb_bin),
    pb_payload_sha256=sha(pb_bin),
    pb_output_symbols={{k: hex(pb_symbols[k]) for k in (
        'pb_output_initialize_global', 'pb_output_valid',
        'pb_output_target_enabled', 'pb_output_target_enable',
        'pb_output_backing_rectangle'
    )}},
    evf_core_changes=evf_changes,
    evf_pool6_patches=evf_pool6_patches,
    boot_panic03_cardslot=boot_panic03_cardslot_meta,
    c64_imenu_ui=c64_imenu_ui_meta,
    nfp_v050=nfp_v050_meta,
    xm6_elf0=xm6_elf0_meta,
    portrait52_records=len(portrait52_changes),
    portrait53_records=len(portrait53_changes),
    crop_aspect_overlays=crop_aspect_overlays,
    zero_banks_verified=[dict(name=n, start=hex(s), end_exclusive=hex(e), bytes=e-s) for n, s, e in zero_banks],
    guards_match_v68={{'0x401fe20c': '61b4d454', '0x401fe280': '5cd62b14', 'cardslot_v71r4_0x401fe300_0x401feeb8': True}},
)
(OUT / 'stage0-base.json').write_text(json.dumps(meta, indent=2) + '\\n')
print('STAGE0_OK ' + json.dumps({{'sha256': meta['sha256'], 'build_id': meta['build_id'], 'pb_bytes': len(pb_bin), 'pb_free': meta['pb_payload_free'], 'evf_pool6_patches': evf_pool6_patches, 'pb_output_symbols': meta['pb_output_symbols']}}))
PY
'''

    res = subprocess.run(
        ['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=8', 'win-lab', 'wsl -e bash -s'],
        input=remote_script, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    )
    print(res.stdout, end='')
    if res.returncode != 0:
        raise SystemExit(res.returncode)

    # Fetch stage0 artifacts back to Mac cleanly via tar-over-ssh
    fetch_script = f'''python3 - <<'PY'
import base64, io, sys, tarfile
from pathlib import Path
out = Path({REMOTE_ROOT!r}) / 'out'
buf = io.BytesIO()
files = {{
    'final/stage0-base.main': out / 'stage0-base.main',
    'final/stage0-base.json': out / 'stage0-base.json',
    'build/pb.elf': out / 'pb/payload.elf',
    'build/pb.bin': out / 'pb/payload.bin',
    'build/pb-ranges.json': out / 'pb/changed-ranges.json',
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

    meta = json.loads((FINAL / 'stage0-base.json').read_text())
    assert sha((FINAL / 'stage0-base.main').read_bytes()) == meta['sha256']
    print("Fetched Stage 0 artifacts cleanly:", FINAL / 'stage0-base.main', meta['sha256'])


if __name__ == '__main__':
    main()
