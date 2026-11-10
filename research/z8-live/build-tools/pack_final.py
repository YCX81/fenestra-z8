#!/usr/bin/env python3
"""Pack a composed Z8 C3.11 MAIN (identity 04.05.p0) into a camera update package.

Same container path as bridge64/live-20261104/pack_combined.py, run on the Mac with the
NikonFirmwarePatch bundle checkout. Nothing is flashed here.
Usage: pack_final.py --main M --sha PREFIX --nfp DIR --stock Z_8_0311.bin --card DIR
                     --readme FILE --qemu JSON --out DIR --name NAME
"""
import argparse
import binascii
import hashlib
import json
import shutil
import sys
import tempfile
import zipfile
from pathlib import Path

sha = lambda b: hashlib.sha256(b).hexdigest()
STOCK = 'd67413c2ede6b9ce8887175f20700bd9fbad2a973a4820968c45503d15d80d96'


def main():
    ap = argparse.ArgumentParser()
    for k in ('main', 'sha', 'nfp', 'stock', 'card', 'readme', 'qemu', 'out', 'name'):
        ap.add_argument('--' + k, required=True)
    ap.add_argument('--layers', default='{}', help='JSON describing the stacked layers')
    a = ap.parse_args()
    sys.path.insert(0, a.nfp)
    from z8prores.package import Package
    from verify import decode

    main_img, raw = Path(a.main).read_bytes(), Path(a.stock).read_bytes()
    assert sha(main_img).startswith(a.sha), 'MAIN hash differs from the expected build'
    assert sha(raw) == STOCK, 'stock container differs'
    assert main_img[0x14eaa90:0x14eaa98] == main_img[-9:-1] == b'04.05.p0', 'identity'
    packet, _ = Package(raw).replace_eg(main_img + binascii.crc_hqx(main_img, 0).to_bytes(2, 'big'),
                                        'eg2070_0405p0.bi')
    x, y = decode(raw), decode(packet)
    assert x[:112] == y[:112] and x[128:581844] == y[128:581844] and x[82389555:-16] == y[82389555:-16]
    assert y[112:128] == b'eg2070_0405p0.bi' and y[581844:82389553] == main_img

    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    final = out / a.name
    assert not final.exists(), f'{final} exists'
    with tempfile.TemporaryDirectory(dir=out) as tmp:
        d = Path(tmp) / 'delivery'
        d.mkdir()
        (d / 'Z_8_0405.bin').write_bytes(packet)
        shutil.copytree(a.card, d / 'FENESTRA')
        shutil.copy(a.readme, d / 'README.txt')
        (d / 'verification.json').write_text(json.dumps(dict(
            sha256=sha(packet), bytes=len(packet), source=STOCK, main=sha(main_img), identity='04.05.p0',
            eg=y[112:128].decode(), layers=json.loads(a.layers), qemu_regression=json.loads(Path(a.qemu).read_text()),
            body_crc=True, header_digest=True, physical_camera_verified=False), indent=2, ensure_ascii=False) + '\n')
        (d / 'SHA256SUMS.txt').write_text(''.join(
            f'{sha(f.read_bytes())}  {f.relative_to(d).as_posix()}\n' for f in sorted(d.rglob('*')) if f.is_file()))
        d.rename(final)
    z = out / (a.name + '.zip')
    with zipfile.ZipFile(z, 'x', zipfile.ZIP_DEFLATED) as zf:
        for f in sorted(final.rglob('*')):
            if f.is_file():
                zf.write(f, f.relative_to(out).as_posix())
    print(final, sha(packet), z, z.stat().st_size)


if __name__ == '__main__':
    main()
