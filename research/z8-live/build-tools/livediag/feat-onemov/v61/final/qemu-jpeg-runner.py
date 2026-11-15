#!/usr/bin/env python3
"""One isolated native JPEG -> Live movie -> JPEG regression; lab paths only.

The existing runner exposes one virtual NVMe card. This is not a dual-card test.
"""
import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import subprocess


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--main', type=Path, required=True)
    ap.add_argument('--pb-manifest', type=Path, required=True)
    ap.add_argument('--layout', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--card-template', type=Path,
                    default=Path('/path/to/user/fxfix/onemov/pom8f-card.raw'))
    ap.add_argument('--qemu', type=Path,
                    default=Path('/path/to/user/fxfix/v53-tx16-counter-rdw-v3/qemu-system-aarch64'))
    ap.add_argument('--seconds', type=int, default=1500)
    ap.add_argument('--virtual-seconds', type=int, default=44)
    a = ap.parse_args()
    root = a.output.resolve()
    root.mkdir(parents=True, exist_ok=False)
    card = root / 'private-card.raw'
    subprocess.run(['cp', '--reflink=auto', str(a.card_template.resolve()), str(card)], check=True)
    contacts = root / 'contacts.json'
    contacts.write_text(json.dumps([
        {'at': 20, 'property': 'mcu-contacts', 'value': '23.5=0'},
        {'at': 20.4, 'property': 'mcu-contacts', 'value': '23.5=1'},
        {'at': 26, 'property': 'mcu-contacts', 'value': '22.12=0'},
        {'at': 26.4, 'property': 'mcu-contacts', 'value': '22.12=1'},
    ], indent=2) + '\n')
    symbols = json.loads(a.pb_manifest.read_text())['symbols']
    observe = symbols['pb_nb_observe']
    points = [observe, 0x406364D0, 0x403AD6A4, 0x403F6FAC, 0x40711328,
              0x4085FCC4, 0x402DD4F8, 0x40CFC820, 0x40CF7D0C,
              0x403F6E88, 0x403F6F10, 0x4063B95C, 0x403F7244, 0x403EC3A0]
    env = dict(os.environ, NB_PEER='1', NB_OUT=str(root / 'trace.jsonl'),
               NB_PTS=':'.join('%x' % p for p in points),
               Z8_TOLERATE='15431000:15432000')
    command = [
        '/path/to/user/.local/share/nikon-z8-qemu/venv/bin/python', '-B', '-u',
        '/path/to/user/fxfix/onemov/run-nb.py', '--firmware', str(a.main.resolve()),
        '--qemu', str(a.qemu.resolve()), '--raw', '/path/to/Nikon/z8-qemu/.local/input.raw',
        '--mcu', '/path/to/user/fxfix/exmcu/EX.bi', '--output', str(root / 'run'),
        '--mcu-pins', 'G1=1:G13=1:G14=1:G15=1', '--experimental-shared221',
        '--experimental-boundary-inputs', '--experimental-sensor-source-clock',
        '--experimental-yuv-channels', '--experimental-movie-selector16',
        '--card', str(card), '--experimental-playback', '--experimental-movie-playback',
        '--movie-audio-output', 'none', '--contact-replay', str(contacts),
        '--seconds', str(a.seconds), '--virtual-seconds', str(a.virtual_seconds),
    ]
    identity = {'main_sha256': hashlib.sha256(a.main.read_bytes()).hexdigest(),
                'qemu_sha256': hashlib.sha256(a.qemu.read_bytes()).hexdigest(),
                'card_template': str(a.card_template.resolve()),
                'private_card': str(card), 'command': command,
                'scope': 'Single NVMe model, pre-existing JPEG/MOV/MAN; native entry and return. Not same-boot capture or dual-card hardware.'}
    (root / 'identity.json').write_text(json.dumps(identity, indent=2) + '\n')
    with (root / 'run.log').open('w') as log:
        status = subprocess.run(command, env=env, cwd='/path/to/Nikon/rdwing-z8-qemu',
                                stdout=log, stderr=subprocess.STDOUT).returncode
    result_path = root / 'run/result.json'
    result = json.loads(result_path.read_text()) if result_path.exists() else {}
    offsets = json.loads(a.layout.read_text())['offsets']
    fields = ['nb_stage', 'nb_photo', 'nb_movie', 'nb_starts', 'nb_returns',
              'nb_redirects', 'nb_w', 'nb_h', 'nb_bit', 'nb_fail', 'nb_cleanups']
    observations = []
    pages, settings, counts = [], [], Counter()
    trace_path = root / 'trace.jsonl'
    if trace_path.exists():
        for line in trace_path.read_text().splitlines():
            row = json.loads(line)
            pc = int(row['pc'], 16)
            regs = [int(v, 16) for v in row['regs']]
            counts[hex(pc)] += 1
            if pc == observe:
                raw = bytes.fromhex(row.get('x0_mem', ''))
                obs = {'where': regs[1]}
                for name in fields:
                    at = offsets[name] - offsets['nb_request']
                    if at >= 0 and at + 4 <= len(raw):
                        obs[name] = int.from_bytes(raw[at:at + 4], 'little')
                for name in ['nb_from', 'nb_to']:
                    at = offsets[name] - offsets['nb_request']
                    if 0 <= at < len(raw):
                        obs[name] = raw[at:at + 64].split(b'\0')[0].decode(errors='replace')
                observations.append(obs)
            elif pc == 0x403AD6A4:
                pages.append(regs[:2])
            elif pc == 0x403EC3A0 and regs[0] == 0x4910:
                settings.append(regs[1])
    entered = [o for o in observations if o['where'] == 2 and o.get('nb_stage') == 2]
    returned = [o for o in observations if o['where'] == 4 and o.get('nb_stage') == 3]
    cleaned = [o for o in observations if o['where'] == 3 and o.get('nb_stage') == 0]
    stderr_path = root / 'run/stderr.log'
    stderr_text = stderr_path.read_text() if stderr_path.exists() else ''
    decoded_pictures = stderr_text.count('Z8_WAVE5_DEC_PICTURE ')
    checks = {
        'runner_exit_zero': status == 0,
        'correct_main_executed': result.get('firmware_sha256') == identity['main_sha256'],
        'native_checks_passed': bool(result.get('checks')) and all(x.get('passed') for x in result['checks']),
        'no_runner_error': result.get('error') is None,
        'bridge_entered': bool(entered), 'bridge_returned': bool(returned),
        'bridge_cleaned': bool(cleaned),
        'open_redirected': bool(cleaned and cleaned[-1].get('nb_redirects', 0) > 0),
        'temporary_bit_released': bool(cleaned and cleaned[-1].get('nb_bit') == 0),
        'original_photo_selected_on_return': bool(entered and entered[-1]['nb_photo'] in settings),
        'movie_page_observed': [3, 0x56C] in pages,
        'photo_page_observed': [3, 0x552] in pages,
        'movie_pictures_decoded': decoded_pictures >= 90,
    }
    summary = {'identity': identity, 'checks': checks, 'passed': all(checks.values()),
               'native_checks_count': len(result.get('checks', [])),
               'native_failed_checks': [x for x in result.get('checks', []) if not x.get('passed')],
               'observations': observations, 'pages': pages, 'selected_ids': settings,
               'point_counts': dict(counts), 'runner_error': result.get('error')}
    summary['decoded_pictures'] = decoded_pictures
    (root / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps({'passed': summary['passed'], 'checks': checks, 'output': str(root)}))
    raise SystemExit(0 if summary['passed'] else 1)


if __name__ == '__main__':
    main()
