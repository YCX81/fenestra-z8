#!/usr/bin/env python3
"""Copy the verified V62 package to the previously authorized SD card only."""
from pathlib import Path
import hashlib
import json
import os
import plistlib
import subprocess

ROOT = Path(__file__).resolve().parent
CARD = Path('/Volumes/NIKON Z 8  ')
UUID = '9D59454C-7746-3814-8B03-E2B56691AE15'
OLD_SHA = '9c9654c694d61a8c4b289e49ededdc4048a32155e34e710d7e3637702c8f767d'
MAIN_SHA = '36c564c9e785822ccf784816882d5055ee8e5ca0b796e1c4fb8e459dfc1ae4b5'
OLD_BACKUP = Path('/path/to/Nikon/delivery/Z8-v61-Live-RAWPeer-db0c847b/Z_8_0406.bin')


def sha(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for b in iter(lambda: f.read(1024 * 1024), b''):
            h.update(b)
    return h.hexdigest()


def card_info():
    d = plistlib.loads(subprocess.check_output(['diskutil', 'info', '-plist', str(CARD)]))
    assert d['VolumeUUID'] == UUID and d['MountPoint'] == str(CARD)
    assert d['BusProtocol'] == 'Secure Digital' and d['RemovableMedia']
    assert d['FilesystemType'] == 'exfat' and d['Writable']
    assert d['TotalSize'] == 62508892160
    return d


def preserved_files():
    """Hash configuration bytes and fingerprint photo/movie metadata without writing."""
    config = {p.relative_to(CARD).as_posix(): sha(p)
              for p in sorted((CARD / 'FENESTRA').rglob('*')) if p.is_file()}
    media = []
    for name in ('DCIM', 'LIVE', 'LIVEDATA'):
        for p in sorted((CARD / name).rglob('*')):
            st = p.stat()
            media.append([p.relative_to(CARD).as_posix(), p.is_dir(), st.st_size, st.st_mtime_ns])
    return {'configuration_sha256': config, 'media_entries': len(media),
            'media_metadata_sha256': hashlib.sha256(json.dumps(media).encode()).hexdigest()}


def main():
    package = json.loads((ROOT / 'final/package.json').read_text())
    assert package['main_sha256'] == MAIN_SHA
    folder = Path(package['directory'])
    verification = json.loads((folder / 'verification.json').read_text())
    assert verification['release_checks_passed'] is True
    assert verification['main_sha256'] == MAIN_SHA
    source, destination = folder / 'Z_8_0406.bin', CARD / 'Z_8_0406.bin'
    assert sha(source) == package['package_sha256'] == verification['package_sha256']
    assert sha(OLD_BACKUP) == OLD_SHA, 'Previous package backup changed'
    info = card_info()
    assert sha(destination) == OLD_SHA, 'Card no longer contains the known V61 package'
    assert info['FreeSpace'] > source.stat().st_size + 1024 * 1024
    before = preserved_files()
    temporary = CARD / '.Z_8_0406.bin.v62-copying'
    assert not temporary.exists(), 'Staging path exists; inspect before retry'
    with source.open('rb') as reader, temporary.open('xb') as writer:
        for block in iter(lambda: reader.read(1024 * 1024), b''):
            writer.write(block)
        writer.flush()
        os.fsync(writer.fileno())
    assert sha(temporary) == package['package_sha256'], 'Staged copy hash mismatch'
    assert card_info()['DeviceIdentifier'] == info['DeviceIdentifier']
    assert sha(destination) == OLD_SHA, 'Destination changed during copy'
    os.replace(temporary, destination)
    os.sync()
    assert sha(destination) == package['package_sha256'], 'Readback hash mismatch'
    after = preserved_files()
    assert before == after, 'Configuration or media metadata changed during copy'
    receipt = {'copied': True, 'safely_ejected': False, 'volume_uuid': UUID,
               'device': info['DeviceIdentifier'], 'whole_disk': info['ParentWholeDisk'],
               'destination': str(destination), 'package_sha256': sha(destination),
               'previous_package_sha256': OLD_SHA, 'previous_backup': str(OLD_BACKUP),
               'main_sha256': MAIN_SHA, 'preserved': after,
               'scope': 'Replaced only root firmware; no camera/flash operation.'}
    (ROOT / 'final/sd-copy.json').write_text(json.dumps(receipt, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps(receipt, ensure_ascii=False))


if __name__ == '__main__':
    main()
