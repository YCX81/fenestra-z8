#!/usr/bin/env python3
"""Build the v37 MAIN (and optionally the update container) on top of a finished
from-stock v36 reconstruction (research/z8-live/from-stock/build.py).

v37 = v36 with three changes, all in the two Live layers:
  * one Live MOV per shot: no 47 MB LIVEDATA/.LVP and no DCIM/LIV_nnnn.MOV copy; the
    in-camera preview is a block of JPEG pictures in a trailing free atom of that MOV;
  * playback reads the preview from the MOV (legacy LVP files still play);
  * the LIVE indicator and the playback badge repaint and re-attach by themselves.

The seed image, its manifest and the unpacked NFP toolkit are taken from the v36
output directory, so every layer below Live is byte-identical to v36. Nothing here
connects to a camera, writes a card or runs QEMU; the result is NOT hardware verified.

usage: build.py --v36-dir DIR --output-dir NEW_DIR [--stock Z_8_0311.bin] [--live-clang clang-21]
                [--llvm-bin DIR]"""
import argparse, hashlib, importlib.util, json, os, re, shutil, subprocess, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
RELEASE = HERE.parent
ARCHIVE = RELEASE.parents[2]
SEED = '50f2a5b22217710c6f35faf1e4639ef675f2738184525be0190f36bcc42cb36b'
LIVE_START, LIVE_LIMIT = 0x401e2000, 0x401fe000     # bootguard helper follows
PB_START, PB_LIMIT = 0x40196000, 0x401a0000


def sha(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


READER = ('lp_preview_open', 'lp_preview_index', 'lp_clip_tail', 'lp_clip_open', 'lp_clip_audio', 'lp_jpeg_decode')


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
    ap.add_argument('--output-dir', type=Path, required=True)
    ap.add_argument('--stock', type=Path, help='original Z_8_0311.bin; builds the update container too')
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
    receipt = dict(schema='v37.build/v1', seed_sha256=SEED, compiler=version.splitlines()[0], stages=[],
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
        shutil.copytree(RELEASE / name, source / name, ignore=shutil.ignore_patterns('__pycache__', '*.o', '*.log'))
    # v37 adds one Live unit (the preview reader the playback layer calls); the v36 builder is used unchanged otherwise.
    builder = out / 'relive_v37.py'
    text = (RELEASE / 'relive_w5.py').read_text()
    assert text.count("'codec/preview_writer.c',") == 1
    builder.write_text(text.replace("'codec/preview_writer.c',", "'codec/preview_writer.c', 'codec/preview_reader.c',"))
    run('live', [sys.executable, '-B', builder, source / 'src-fix37', seed, seed_manifest, out / 'live'])
    live_bytes = (out / 'live/payload.bin').stat().st_size
    assert live_bytes <= LIVE_LIMIT - LIVE_START, 'Live payload overlaps the bootguard helper'
    run('playback', [sys.executable, '-B', ARCHIVE / 'build-tools/pb/relive_pb.py', source / 'pb-fix37',
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
    receipt.update(live_payload_bytes=live_bytes, live_free=LIVE_LIMIT - LIVE_START - live_bytes,
                   playback_payload_bytes=pb_bytes, playback_free=PB_LIMIT - PB_START - pb_bytes)
    if args.stock:
        toolkit = next((v36 / 'dependencies/rc2').iterdir())
        package = out / 'Z_8_0407.bin'
        run('package', [sys.executable, '-B', ARCHIVE / 'from-stock/nfp_backend.py', 'pack', '--toolkit',
                        toolkit, '--stock', args.stock.resolve(), '--main', final, '--output', package])
        record('package', package)
    (out / 'build.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps({k: receipt[k] for k in ('live_payload_bytes', 'live_free', 'playback_payload_bytes',
                                              'playback_free')}))


if __name__ == '__main__':
    main()
