#!/usr/bin/env python3
"""Compile v53 candidate source layers with a bounded shutter bank using the pinned v36 build seed.
The output v37.main is an intermediate, never a complete firmware candidate:
compose.py must apply the separately linked extra code, preserved v51 overlays,
EVF core adapters and portrait records. Only pack53.py may package a verified
composed MAIN. No camera, card or updater operations are supported here.
"""
import argparse, hashlib, importlib.util, json, os, re, shutil, subprocess, sys
from pathlib import Path

HERE = Path('/path/to/user/v51src/research/z8-live/build-tools/livediag/feat-release/tools-v37')
RELEASE = HERE.parent
ARCHIVE = RELEASE.parents[2]
SEED = '50f2a5b22217710c6f35faf1e4639ef675f2738184525be0190f36bcc42cb36b'
LIVE_START, LIVE_LIMIT = 0x401e2000, 0x401fd000     # bootguard helper follows
PB_START, PB_LIMIT = 0x40196000, 0x4019f020
SHUTTER_START, SHUTTER_LIMIT = 0x4019f020, 0x4019ff40


def sha(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


READER = ('lp_preview_open', 'lp_preview_index', 'lp_clip_tail', 'lp_clip_open', 'lp_clip_audio', 'lp_jpeg_decode', 'lp_audio_output_profile')


def pb_defs(live_manifest):
    """Link symbols for the playback layer: the two core helpers as in v36, plus the
    preview reader functions that live in the Live payload."""
    symbols = json.loads(Path(live_manifest).read_text())['symbols']
    defs = {'fx_glyph': '0x401f0000', 'fx_memmove': '0x401f0100'}
    defs.update({name: hex(symbols[name]) for name in READER})
    return defs


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--v36-dir', type=Path, required=True)
    ap.add_argument('--source-dir', type=Path, required=True)
    ap.add_argument('--output-dir', type=Path, required=True)
    ap.add_argument('--live-clang', default='clang-21')
    ap.add_argument('--llvm-bin', type=Path)
    args = ap.parse_args()
    v36 = args.v36_dir.resolve()
    seed, seed_manifest = v36 / 'seed.main', v36 / 'seed-manifest.json'
    assert sha(seed) == SEED, 'not the v36 from-stock seed'
    clang = shutil.which(args.live_clang)
    assert clang, 'missing ' + args.live_clang
    clang = str(Path(clang).resolve())
    version = subprocess.check_output([clang, '--version'], text=True)
    assert re.search(r'clang version 21\.', version), version.splitlines()[0]
    llvm_bin = (args.llvm_bin or Path(clang).parent).resolve()
    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=False)
    logs = out / 'logs'
    logs.mkdir()
    env = dict(os.environ, V36_REAL_CLANG=clang,
               PATH=os.pathsep.join([str(HERE / 'compiler'), str(llvm_bin), os.environ['PATH']]))
    receipt = dict(schema='v53.build/v1', seed_sha256=SEED, compiler=version.splitlines()[0], stages=[],
                   physical_camera_verified=False)

    def run(name, command):
        with (logs / (name + '.log')).open('w') as log:
            r = subprocess.run(list(map(str, command)), env=env, stdout=log, stderr=subprocess.STDOUT)
        if r.returncode:
            sys.stderr.write((logs / (name + '.log')).read_text()[-4000:])
            raise SystemExit(name + ' failed')

    def record(name, path):
        receipt['stages'].append(dict(stage=name, file=str(path.relative_to(out)), bytes=path.stat().st_size,
                                      sha256=sha(path)))
        (out / 'build.json').write_text(json.dumps(receipt, indent=2) + '\n')
        print(name + ': ' + receipt['stages'][-1]['sha256'], flush=True)

    source = out / 'work'
    for name in ('src-fix37', 'pb-fix37'):
        shutil.copytree(args.source_dir / name, source / name, ignore=shutil.ignore_patterns('__pycache__', '*.o', '*.log'))
    # v37 adds one Live unit (the preview reader the playback layer calls); the v36 builder is used unchanged otherwise.
    builder = out / 'relive_v37.py'
    text = (RELEASE / 'relive_w5.py').read_text()
    assert text.count("'codec/preview_writer.c',") == 1
    text = text.replace("'codec/preview_writer.c',", "'codec/preview_writer.c', 'codec/preview_reader.c', 'codec/live_hevc.c',")
    assert text.count("'audio/live_audio_z8.c'") == 1
    text = text.replace("'audio/live_audio_z8.c'", "'audio/live_audio_z8.c', 'audio/live_shutter_z8.c'")
    # Keep the 401fd000 size/meta helpers untouched. Extra code is linked at
    # its real address but composed separately, never as a binary with a hole.
    text = text.replace('LIMIT = 0x401fff80', 'LIMIT = 0x401fd000')
    anchor = "ld = out / 'payload.ld'"
    assert text.count(anchor) == 1
    text = text.replace(anchor, "defs.update(live_stock_shutter_prepare=0x40f9cccc, live_stock_shutter_fire=0x40f9cd54)\n" + anchor)
    # Link sections in increasing address order; a backward RX section makes
    # ld.lld's automatic PT_LOAD file offsets underflow.
    bank_anchor = 'SECTIONS {{ . = {START:#x};'
    assert text.count(bank_anchor) == 1
    text = text.replace(bank_anchor, 'SECTIONS {{ .live_shutter 0x4019f020 : {{ *(.live_shutter) }}\nASSERT(. <= 0x4019ff40, "Live shutter bank exhausted")\n. = {START:#x};')
    anchor = 'ASSERT(. <= {LIMIT:#x}, "RX padding exhausted")'
    assert text.count(anchor) == 1
    text = text.replace(anchor, anchor + '\n.live_extra 0x401fd218 : {{ *(.live_extra) }}\nASSERT(. <= 0x401fe000, "Live extra cave exhausted")')
    anchor = "subprocess.run(['llvm-objcopy', '-O', 'binary', str(elf), str(blob)], check=True)"
    assert text.count(anchor) == 1
    splitter = str(Path(__file__).resolve().with_name('split_live_payload_v53.py'))
    text = text.replace(anchor, f"import importlib.util\nsp=importlib.util.spec_from_file_location('v52split',{splitter!r})\nsm=importlib.util.module_from_spec(sp);sp.loader.exec_module(sm)\nparts,_,sections=sm.split(elf)\nblob.write_bytes(parts[0])\n(out/'extra.bin').write_bytes(parts[1])\n(out/'shutter.bin').write_bytes(parts[2])\n(out/'extra-sections.json').write_text(json.dumps(sections,indent=2)+'\\n')")
    builder.write_text(text)
    run('live', [sys.executable, '-B', builder, source / 'src-fix37', seed, seed_manifest, out / 'live'])
    live_bytes = (out / 'live/payload.bin').stat().st_size
    assert live_bytes <= LIVE_LIMIT - LIVE_START, 'Live payload overlaps the bootguard helper'
    pb_builder = out / 'relive_pb53.py'
    pb_text = (ARCHIVE / 'build-tools/pb/relive_pb.py').read_text()
    assert pb_text.count('0x401a0000') == 1
    pb_builder.write_text(pb_text.replace('0x401a0000','0x4019f020'))
    run('playback', [sys.executable, '-B', pb_builder, source / 'pb-fix37',
                     out / 'live/live-fixed.emulation-only.main', out / 'live/changed-ranges.json', out / 'pb',
                     '--defs=' + json.dumps(pb_defs(out / 'live/changed-ranges.json'))])
    playback = out / 'pb/live-fixed.emulation-only.main'
    pb_bytes = (out / 'pb/payload.bin').stat().st_size
    record('playback', playback)
    (out / 'usb').mkdir()
    usb = out / 'usb/usb.main'
    run('usb', [sys.executable, '-B', ARCHIVE / 'build-tools/usb_default_on.py', playback, usb])
    spec = importlib.util.spec_from_file_location('v36_chain', RELEASE / 'tools-v36/chain.py')
    chain = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(chain)
    manifest = chain.append_changes(playback.read_bytes(), usb.read_bytes(),
                                    json.loads((playback.parent / 'changed-ranges.json').read_text()), 'USB default on')
    (usb.parent / 'changed-ranges.json').write_text(json.dumps(manifest, indent=2) + '\n')
    record('usb', usb)
    (out / 'pool').mkdir()
    pool = out / 'pool/pool.main'
    run('pool', [sys.executable, '-B', ARCHIVE / 'build-tools/gui_pool_fix.py', usb, pool])
    record('pool', pool)
    final = out / 'v37.main'
    run('bootguard', [sys.executable, '-B', RELEASE / 'tools-v36/bootguard_patch.py', pool, final])
    record('main', final)
    extra_bytes = (out / 'live/extra.bin').stat().st_size
    shutter_bytes = (out / 'live/shutter.bin').stat().st_size
    assert shutter_bytes <= SHUTTER_LIMIT - SHUTTER_START
    receipt.update(shutter_bytes=shutter_bytes, shutter_free=SHUTTER_LIMIT-SHUTTER_START-shutter_bytes, live_payload_bytes=live_bytes, live_free=LIVE_LIMIT - LIVE_START - live_bytes,
                   live_extra_bytes=extra_bytes, live_extra_free=0x401fe000-0x401fd218-extra_bytes,
                   playback_payload_bytes=pb_bytes, playback_free=PB_LIMIT - PB_START - pb_bytes)
    (out / 'build.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps({k: receipt[k] for k in ('live_payload_bytes', 'live_free', 'playback_payload_bytes',
                                              'playback_free')}))


if __name__ == '__main__':
    main()
