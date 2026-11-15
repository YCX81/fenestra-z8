"""Install the verified candidate BIN only; retain all card media and settings."""
from pathlib import Path
import argparse
import hashlib
import json
import os

ROOT = Path(__file__).resolve().parent
OLD_SHA = 'e4a61d2af3480f66b37a83ec967607a179f18fbbc454c2df7673285e80890f0f'
NEW_SHA = '6eeee8f583adbf02dc45ac5ed6f0713c71efb5e1cd016d8b42027b2a143e0fc0'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def protected_state(card):
    rows = {}
    for name in ('DCIM', 'FENESTRA', 'LIVEDATA'):
        for p in sorted((card / name).rglob('*')):
            if p.is_file():
                stat = p.stat()
                row = dict(bytes=stat.st_size, mtime_ns=stat.st_mtime_ns)
                if name == 'FENESTRA':
                    row['sha256'] = sha(p.read_bytes())
                rows[str(p.relative_to(card))] = row
    return rows


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('package', type=Path)
    parser.add_argument('--card', type=Path, default=Path('/Volumes/NIKON Z 8  '))
    parser.add_argument('--receipt', type=Path, required=True)
    args = parser.parse_args()
    assert not args.receipt.exists()
    v = json.loads((args.package / 'verification.json').read_text())
    assert v['release_status'] == 'qemu_test_candidate'
    assert v['core_regression_passed'] and v['binary_audit_passed']
    assert v['rec_video_sequence_passed'] and v['legacy_rec_video_sequence_passed']
    assert v['independent_review']['passed'] is True
    assert v['unresolved_pre_rec_gui_freeze_observed'] is True
    payload = (args.package / 'Z_8_0406.bin').read_bytes()
    assert sha(payload) == v['package_sha256'] == NEW_SHA
    assert sha((ROOT / 'card-backup-before-v53g/Z_8_0406-v52.bin').read_bytes()) == OLD_SHA
    target = args.card / 'Z_8_0406.bin'
    assert target.is_file() and not target.is_symlink()
    assert sha(target.read_bytes()) == OLD_SHA, 'Card firmware changed since backup'
    before = protected_state(args.card)
    backup = json.loads((ROOT / 'card-backup-before-v53g/inventory.json').read_text())
    for row in backup['files']:
        if row['path'].startswith('FENESTRA/') and 'sha256' in row:
            assert before[row['path']]['sha256'] == row['sha256'], 'Card settings changed'
    temporary = args.card / '.Z_8_0406.v53.part'
    assert not temporary.exists()
    with temporary.open('xb') as f:
        f.write(payload)
        f.flush()
        os.fsync(f.fileno())
    assert sha(temporary.read_bytes()) == NEW_SHA
    os.replace(temporary, target)
    assert sha(target.read_bytes()) == NEW_SHA
    after = protected_state(args.card)
    assert before == after, 'Protected card contents changed during installation'
    record = dict(card=str(args.card), firmware=str(target), bytes=len(payload),
                  prior_firmware_sha256=OLD_SHA, firmware_sha256=NEW_SHA,
                  preserved_files=after, protected_state_equal=True,
                  camera_flashed=False, release_status=v['release_status'],
                  unresolved_pre_rec_gui_freeze_observed=True)
    with args.receipt.open('x') as out:
        json.dump(record, out, ensure_ascii=False, indent=2)
        out.write('\n')
    print(json.dumps(dict(firmware=str(target), sha256=NEW_SHA,
                          preserved_files=len(after), camera_flashed=False)))


if __name__ == '__main__':
    main()
