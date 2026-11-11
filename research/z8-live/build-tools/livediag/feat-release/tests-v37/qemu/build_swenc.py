#!/usr/bin/env python3
"""QEMU-only test image: v37 with the Wave5 encoder unit swapped for the software
H.264 encoder kept in the tree (codec/live_encoder.c, minih264).

Why: the Wave5 model of the local QEMU does not complete the photo-mode bring-up that
v32+ Live uses (stock v36 fails the same way), so no clip is ever produced there. Every
other unit - capture, transaction, MOV writer, preview writer, UI, playback - is the
v37 source unchanged. This image is for emulation only and must never be packaged.

usage: build_swenc.py V36_DIR OUT_DIR   (PATH must provide clang 21 / ld.lld, see tools-v37/make.sh)"""
import json, os, shutil, subprocess, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
RELEASE = HERE.parents[1]
ARCHIVE = RELEASE.parents[2]
v36, out = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
SRC, PB = (sys.argv[3], sys.argv[4]) if len(sys.argv) > 4 else ('src-fix37', 'pb-fix37')   # control: src-fix36 pb-fix36
out.mkdir(parents=True)
src = out / 'work/src-fix37'
shutil.copytree(RELEASE / SRC, src, ignore=shutil.ignore_patterns('__pycache__', '*.o', '*.log'))
shutil.copytree(RELEASE / PB, out / 'work/pb-fix37', ignore=shutil.ignore_patterns('__pycache__', '*.o', '*.log'))
enc = src / 'codec/live_encoder.c'
enc.write_text(enc.read_text() + '''
/* test-only: the Wave5 unit's extra entry points */
void live_encoder_set_flags(live_encoder *e,unsigned flags){(void)e;(void)flags;}
void live_encoder_preempt(live_encoder *e,uint32_t client){(void)e;(void)client;}
''')
builder = out / 'relive_swenc.py'
text = (RELEASE / 'relive_w5.py').read_text()
old = "'codec/live_w5enc.c', 'codec/live_h264_idr.c',"
assert text.count(old) == 1
text = text.replace(old, "'codec/live_encoder.c',")
reader = (src / 'codec/preview_reader.c').exists()   # v37 only; the v36 control has no such unit
if reader:
    text = text.replace("'codec/preview_writer.c',", "'codec/preview_writer.c', 'codec/preview_reader.c',")
builder.write_text(text)
env = dict(os.environ, V36_REAL_CLANG=shutil.which('clang'),
           PATH=str(RELEASE / 'tools-v37/compiler') + os.pathsep + os.environ['PATH'])
run = lambda *c: subprocess.run(list(map(str, c)), check=True, env=env)
run(sys.executable, '-B', builder, src, v36 / 'seed.main', v36 / 'seed-manifest.json', out / 'live')
defs = {'fx_glyph': '0x401f0000', 'fx_memmove': '0x401f0100'}
if reader:
    symbols = json.loads((out / 'live/changed-ranges.json').read_text())['symbols']
    defs.update({n: hex(symbols[n]) for n in ('lp_preview_open', 'lp_preview_index', 'lp_clip_tail', 'lp_clip_open',
                                              'lp_clip_audio', 'lp_jpeg_decode')})
run(sys.executable, '-B', ARCHIVE / 'build-tools/pb/relive_pb.py', out / 'work/pb-fix37',
    out / 'live/live-fixed.emulation-only.main', out / 'live/changed-ranges.json', out / 'pb',
    '--defs=' + json.dumps(defs))
(out / 'usb').mkdir(); (out / 'pool').mkdir()
run(sys.executable, '-B', ARCHIVE / 'build-tools/usb_default_on.py', out / 'pb/live-fixed.emulation-only.main', out / 'usb/usb.main')
import importlib.util
spec = importlib.util.spec_from_file_location('v36_chain', RELEASE / 'tools-v36/chain.py')
chain = importlib.util.module_from_spec(spec); spec.loader.exec_module(chain)
manifest = chain.append_changes((out / 'pb/live-fixed.emulation-only.main').read_bytes(), (out / 'usb/usb.main').read_bytes(),
                                json.loads((out / 'pb/changed-ranges.json').read_text()), 'USB default on')
(out / 'usb/changed-ranges.json').write_text(json.dumps(manifest) + '\n')
run(sys.executable, '-B', ARCHIVE / 'build-tools/gui_pool_fix.py', out / 'usb/usb.main', out / 'pool/swenc-test.main')
print(json.dumps(dict(live_payload=(out / 'live/payload.bin').stat().st_size, image=str(out / 'pool/swenc-test.main'),
                      emulation_only=True)))
