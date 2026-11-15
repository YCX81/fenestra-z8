#!/usr/bin/env python3
"""Package a verified V61 MAIN into a fresh offline candidate directory.

No mounted-card, camera, USB or update operations. Requires explicit artifacts;
does not choose an unverified build or mark hardware acceptance.
"""
import argparse
import binascii
import hashlib
import json
from pathlib import Path
import shutil
import sys
import zipfile


def sha(data):
    return hashlib.sha256(data).hexdigest()


def require(ok, message):
    if not ok:
        raise ValueError(message)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--main', type=Path, required=True)
    ap.add_argument('--verification', type=Path, required=True)
    ap.add_argument('--readme', type=Path, required=True)
    ap.add_argument('--fenestra', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--original', type=Path,
                    default=Path('/path/to/Nikon/z8-qemu/.local/firmware/Z_8_0311.bin'))
    ap.add_argument('--patch-library', type=Path,
                    default=Path('/path/to/Nikon/NikonFirmwarePatch'))
    a = ap.parse_args()
    verification = json.loads(a.verification.read_text())
    image = a.main.read_bytes()
    require(len(image) == 81807709, 'Unexpected MAIN size')
    require(sha(image) == verification['main_sha256'], 'Verification refers to a different MAIN')
    require(image[0x14EAA90:0x14EAA98] == image[-9:-1] == b'04.06.p0', 'Identity mismatch')
    original = a.original.read_bytes()
    require(sha(original) == 'd67413c2ede6b9ce8887175f20700bd9fbad2a973a4820968c45503d15d80d96',
            'Original is not the pinned official C3.11 package')
    sys.path.insert(0, str(a.patch_library.resolve()))
    from z8prores.package import Package
    from verify import decode
    packet, _ = Package(original).replace_eg(
        image + binascii.crc_hqx(image, 0).to_bytes(2, 'big'), 'eg2070_0406p0.bi')
    old, new = decode(original), decode(packet)
    require(old[:112] == new[:112] and old[128:581844] == new[128:581844]
            and old[82389555:-16] == new[82389555:-16], 'Unexpected non-EG change')
    require(new[112:128] == b'eg2070_0406p0.bi', 'Incorrect directory identity')
    require(new[581844:82389553] == image, 'Packaged MAIN differs')
    require(new[82389553:82389555] == binascii.crc_hqx(image, 0).to_bytes(2, 'big'), 'MAIN CRC mismatch')
    name = 'Z8-v61-Live-RAWPeer-' + verification['main_sha256'][:8]
    folder, archive = a.output / name, a.output / (name + '.zip')
    require(not folder.exists() and not archive.exists(), 'Output already exists')
    require(a.fenestra.is_dir(), 'FENESTRA source must be a directory')
    folder.mkdir(parents=True)
    (folder / 'Z_8_0406.bin').write_bytes(packet)
    shutil.copytree(a.fenestra, folder / 'FENESTRA')
    shutil.copyfile(a.readme, folder / 'README.txt')
    verification.update(version='v61', identity='04.06.p0', package_sha256=sha(packet),
                        package_bytes=len(packet), physical_camera_verified=False)
    (folder / 'verification.json').write_text(json.dumps(verification, ensure_ascii=False, indent=2) + '\n')
    (folder / 'SHA256SUMS.txt').write_text(''.join(
        sha(p.read_bytes()) + '  ' + p.relative_to(folder).as_posix() + '\n'
        for p in sorted(folder.rglob('*')) if p.is_file()))
    with zipfile.ZipFile(archive, 'x', zipfile.ZIP_DEFLATED) as z:
        for p in sorted(folder.rglob('*')):
            if p.is_file():
                z.write(p, p.relative_to(a.output))
    print(json.dumps({'directory': str(folder), 'zip': str(archive),
                      'main_sha256': verification['main_sha256'], 'package_sha256': sha(packet)}))


if __name__ == '__main__':
    main()
