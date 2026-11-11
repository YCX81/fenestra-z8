"""Replay the v36 laboratory MAIN chain into a new local directory; no device access."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT.parent.parent
LOAD = 0x40160000
PINS = {
    'base': '54f25ad5d58d2c1f26498db7771d33816616e5494eb404f784cf8fe640d77c0e',
    'live': '27d5f1dac7be0426b7747f9ff159b8a4a37e60bf082648e17cba38c5c7ded719',
    'playback': 'a46b334d28b72f8f9899f11731c3537a607144300d399a9d6dac82d7629bced2',
    'usb': '56395e20d394f9f454a4160497df07e2c2e40e97433d82f522378c6ef70b897a',
    'pool': '98638cf72deb1980fb7b3cdf2b82501f112303dad0ba9beb9a6757517b790544',
}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def append_changes(before, after, manifest, purpose):
    if len(before) != len(after):
        raise ValueError('MAIN length changed')
    rows = []
    at = 0
    while at < len(before):
        if before[at] == after[at]:
            at += 1
            continue
        end = at + 1
        while end < len(before) and before[end] != after[end]:
            end += 1
        rows.append(dict(address=hex(LOAD + at), size=end - at, purpose=purpose,
                         expected=before[at:end].hex(), replacement=after[at:end].hex()))
        at = end
    return dict(manifest, ranges=manifest['ranges'] + rows, output_sha256=sha(after))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', type=Path, required=True)
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    original = args.base.read_bytes()
    if sha(original) != PINS['base']:
        parser.error('use the pinned NFP rc.2 merged MAIN')
    manifest = json.loads(args.manifest.read_text())
    if manifest.get('output_sha256') != PINS['base']:
        parser.error('materialize the base manifest recipe first; the historical header is stale')
    real_clang = shutil.which('clang')
    if not real_clang:
        parser.error('clang 21 is required')
    version = subprocess.check_output([real_clang, '--version'], text=True)
    if not re.search(r'clang version 21\.', version):
        parser.error('clang 21 is required')
    for tool in ('ld.lld', 'llvm-nm', 'llvm-objcopy'):
        if not shutil.which(tool):
            parser.error('missing LLVM tool: ' + tool)
    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=False)
    environment = dict(os.environ, V36_REAL_CLANG=real_clang,
                       PATH=str(ROOT / 'tools-v36/compiler') + os.pathsep + os.environ['PATH'])
    receipt = dict(scope='local MAIN only; no update package or device access',
                   compiler=version, stages=[])

    def run(label, command):
        with (out / (label + '.log')).open('w') as log:
            subprocess.run(command, cwd=ROOT, env=environment, stdout=log,
                           stderr=subprocess.STDOUT, check=True)

    def record(label, image):
        digest = sha(image.read_bytes())
        receipt['stages'].append(dict(stage=label, path=str(image), sha256=digest))
        (out / 'chain-receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
        if digest != PINS[label]:
            raise ValueError(label + ' differs from the recorded v36 output: ' + digest)

    run('live', [sys.executable, '-B', str(ROOT / 'relive_w5.py'),
                 str(ROOT / 'src-fix36'), str(args.base.resolve()),
                 str(args.manifest.resolve()), str(out / 'live')])
    live = out / 'live/live-fixed.emulation-only.main'
    if (out / 'live/payload.bin').stat().st_size > 0x401fe000 - 0x401e2000:
        raise ValueError('Live payload overlaps the reserved bootguard helper region')
    record('live', live)
    run('playback', [sys.executable, '-B', str(TOOLS / 'pb/relive_pb.py'),
                     str(ROOT / 'pb-fix36'), str(live), str(out / 'live/changed-ranges.json'),
                     str(out / 'pb'), '--defs={"fx_glyph":"0x401f0000","fx_memmove":"0x401f0100"}'])
    playback = out / 'pb/live-fixed.emulation-only.main'
    record('playback', playback)
    (out / 'usb').mkdir()
    usb = out / 'usb/usb.main'
    run('usb', [sys.executable, '-B', str(TOOLS / 'usb_default_on.py'), str(playback), str(usb)])
    usb_manifest = append_changes(playback.read_bytes(), usb.read_bytes(),
                                 json.loads((out / 'pb/changed-ranges.json').read_text()),
                                 'usb debug default on')
    (out / 'usb/changed-ranges.json').write_text(json.dumps(usb_manifest, indent=2) + '\n')
    record('usb', usb)
    (out / 'pool').mkdir()
    pool = out / 'pool/pool.main'
    run('pool', [sys.executable, '-B', str(TOOLS / 'gui_pool_fix.py'), str(usb), str(pool)])
    final = pool.read_bytes()
    pool_manifest = json.loads((out / 'pool/changed-ranges.json').read_text())
    if pool_manifest['output_sha256'] != sha(final):
        raise ValueError('final cumulative manifest hash differs')
    # Check actual final bytes and every changed address, including helper/GUI steps.
    owned = bytearray(len(final))
    for row in pool_manifest['ranges']:
        at = int(row['address'], 16) - LOAD
        end = at + row['size']
        if at < 0 or end > len(final) or end <= at:
            raise ValueError('invalid cumulative range')
        if final[at:end] != bytes.fromhex(row['replacement']):
            raise ValueError('final replacement differs at ' + row['address'])
        owned[at:end] = b'\1' * (end - at)
    if any(a != b and not allowed for a, b, allowed in zip(original, final, owned)):
        raise ValueError('change outside cumulative manifest')
    record('pool', pool)
    print(json.dumps(receipt, indent=2))


if __name__ == '__main__':
    main()
