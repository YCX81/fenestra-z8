#!/usr/bin/env python3
"""Package verified V67 (Full CJK Font Engine + Chinese UI for Live/C64/Portrait) into delivery dir."""
import argparse
import binascii
import hashlib
import json
import shutil
import sys
import zipfile
from pathlib import Path


def sha(data: bytes) -> str:
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
    ap.add_argument('--name-prefix', type=str, default='Z8-v69-AllInOne-')
    ap.add_argument('--version', type=str, default='v69-allinone')
    ap.add_argument('--original', type=Path,
                    default=Path('/Users/ycx/Nikon/_mac-migration/firmware/Z_8_0311.bin'))
    ap.add_argument('--patch-library', type=Path,
                    default=Path('/Users/ycx/Nikon/external/NikonFirmwarePatch-04.18'))
    a = ap.parse_args()

    verification = json.loads(a.verification.read_text())
    require(verification.get('release_checks_passed') is True, 'Required release checks are incomplete')
    evidence_files = []
    for evidence in verification['evidence']:
        path = a.verification.parent / evidence['path']
        require(sha(path.read_bytes()) == evidence['sha256'], 'Verification evidence changed: ' + str(path))
        require(Path(evidence['name']).name == evidence['name'], 'Invalid evidence name')
        evidence_files.append((path, evidence['name']))

    image = a.main.read_bytes()
    require(len(image) == 81807709, 'Unexpected MAIN size')
    require(sha(image) == verification['main_sha256'], 'Verification refers to a different MAIN')
    require(image[0x14EAA90:0x14EAA98] == image[-9:-1] == b'04.06.p0', 'Identity mismatch')

    original = a.original.read_bytes()
    require(sha(original) == 'd67413c2ede6b9ce8887175f20700bd9fbad2a973a4820968c45503d15d80d96',
            'Original is not the pinned official C3.11 package')

    sys.path.insert(0, str(a.patch_library.resolve()))
    from z8prores.package import Package, transform, AUTH_APPEND, OUTER_FIELD, MODULES
    from verify import decode as verify_decode_orig

    # Recover Nikon Z8 XOR envelope tables from pinned official Z_8_0311.bin
    orig_pkg = Package(original)
    old = verify_decode_orig(original)
    require(old == orig_pkg.body, 'Independent decode mismatch on official Z_8_0311.bin')

    # Construct replacement package body (identical layout/CRC/SHA1 to Package.replace_eg)
    eg_with_crc = image + binascii.crc_hqx(image, 0).to_bytes(2, 'big')
    require(len(eg_with_crc) == 81807711 and binascii.crc_hqx(eg_with_crc, 0) == 0, 'Invalid EG CRC')
    body = bytearray(orig_pkg.body)
    body[112:128] = b'eg2070_0406p0.bi'
    body[581844:581844 + len(eg_with_crc)] = eg_with_crc
    body[-16:-14] = binascii.crc_hqx(body[:-16], 0).to_bytes(2, 'big')
    body_bytes = bytes(body)
    packet = hashlib.sha1(body_bytes + AUTH_APPEND).digest() + OUTER_FIELD + transform(body_bytes, orig_pkg.tables)

    # Independent 3-way verification of the encrypted Z_8_0406.bin envelope:
    # (Note: CJK_EXT_2 occupies 0x40b8a000..0x40bfff6e, which overlaps z8prores.Package's
    # offline-only heuristic zero window 0xB10000; the camera uses fixed XOR tables, and
    # we independently recover those exact tables from BOTH official Z_8_0311.bin AND from
    # packet itself using the untouched 3.45 MiB zero window at body offset 0x11E0000!)
    require(len(packet) == len(original) == 99051117, 'Unexpected package size')
    require(packet[20:32] == OUTER_FIELD, 'Outer field mismatch')

    # Way 1: Independently recover XOR tables from packet itself via untouched zero plane 0x11E0000
    cipher = memoryview(packet)[32:]
    zero_alt, embedded_c = 0x11E0000, 0x3FF1F84
    require(body_bytes[zero_alt:zero_alt + 65536] == bytes(65536), 'Alt zero window not zero')
    a2 = bytes(cipher[zero_alt + x] ^ cipher[zero_alt] for x in range(256))
    b2 = bytes(cipher[zero_alt + 256 * y] ^ cipher[zero_alt] for y in range(256))
    part2 = bytes(cipher[embedded_c + x] ^ a2[(embedded_c + x) & 255] ^
                  b2[((embedded_c + x) >> 8) & 255] for x in range(256))
    shift2 = part2[(zero_alt >> 16) & 255] ^ cipher[zero_alt]
    c2 = bytes(v ^ shift2 for v in part2)
    new = transform(packet[32:], (a2, b2, c2))

    # Way 2: Structural, SHA-1, Body CRC-16, and all 5 Module CRC-16 + non-EG hash checks
    require(new == body_bytes, 'Self-recovered decrypt did not round-trip body')
    require(new[:32] == b' ' * 32, 'Invalid body header')
    import struct as _st
    require(_st.unpack_from('>4I', new, 32) == (5, 208, 0, 0), 'Unsupported module directory')
    require(new[-14:] == bytes(14), 'Invalid trailer')
    require(binascii.crc_hqx(new[:-14], 0) == 0, 'Package body CRC mismatch')
    require(hashlib.sha1(new + AUTH_APPEND).digest() == packet[:20], 'Package SHA1 mismatch')
    for i, (exp_name, start, size, exp_sha) in enumerate(MODULES):
        m_name, ma, mn, r0, r1 = _st.unpack_from('>16s4I', new, 48 + i * 32)
        require((ma, mn, r0, r1) == (start, size, 0, 0), f'Module {i} layout mismatch')
        m_data = new[ma:ma + mn]
        require(binascii.crc_hqx(m_data, 0) == 0, f'Module {i} CRC mismatch')
        if exp_sha is not None:
            require(m_name.rstrip(b'\0').decode('ascii') == exp_name and sha(m_data) == exp_sha,
                    f'Non-EG module {i} modified')

    # Way 3: Exact 99 MB keystream identity check against official Z_8_0311.bin!
    # (packet[32:] ^ original[32:]) MUST equal (new ^ old) at EVERY byte!
    for off in range(0, len(new), 1 << 20):
        end = min(off + (1 << 20), len(new))
        p_xor = int.from_bytes(packet[32 + off:32 + end], 'little') ^ int.from_bytes(original[32 + off:32 + end], 'little')
        b_xor = int.from_bytes(new[off:end], 'little') ^ int.from_bytes(old[off:end], 'little')
        require(p_xor == b_xor, f'Keystream mismatch in chunk {off:#x}..{end:#x}')

    require(old[:112] == new[:112] and old[128:581844] == new[128:581844]
            and old[82389555:-16] == new[82389555:-16], 'Unexpected non-EG change')
    require(new[112:128] == b'eg2070_0406p0.bi', 'Incorrect directory identity')
    require(new[581844:82389553] == image, 'Packaged MAIN differs')
    require(new[82389553:82389555] == binascii.crc_hqx(image, 0).to_bytes(2, 'big'), 'MAIN CRC mismatch')

    name = a.name_prefix + verification['main_sha256'][:8]
    folder, archive = a.output / name, a.output / (name + '.zip')
    require(not folder.exists() and not archive.exists(), 'Output already exists: ' + str(folder))
    require(a.fenestra.is_dir(), 'FENESTRA source must be a directory')

    folder.mkdir(parents=True)
    (folder / 'Z_8_0406.bin').write_bytes(packet)
    shutil.copytree(a.fenestra, folder / 'FENESTRA')
    shutil.copyfile(a.readme, folder / 'README.txt')
    (folder / 'EVIDENCE').mkdir()
    for (source, ev_name), evidence in zip(evidence_files, verification['evidence']):
        target = folder / 'EVIDENCE' / ev_name
        require(not target.exists(), 'Duplicate evidence name')
        shutil.copyfile(source, target)
        evidence['source_path'] = str(source.resolve())
        evidence['path'] = target.relative_to(folder).as_posix()

    verification.update(version=a.version, identity='04.06.p0', package_sha256=sha(packet),
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
                      'main_sha256': verification['main_sha256'], 'package_sha256': sha(packet)}, indent=2))


if __name__ == '__main__':
    main()
