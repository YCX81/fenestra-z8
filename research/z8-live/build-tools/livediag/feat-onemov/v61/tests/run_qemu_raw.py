#!/usr/bin/env python3
"""Private single-card RAW-anchor regression using an explicitly synthetic DCF group.

The added real NEF and existing JPG/Live MOV are NOT from the same exposure.
This tests native RAW record/anchor support, not photographic pairing identity.
"""
import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys

NEF_SHA = 'd519a7bb2b7365a9ede204718d8ab80e9d369829d9767d16a40ccdf5649bf3ee'
MAIN_SHA = 'db0c847b333f0a6e07222f2207435e9bf340ce1d544704f6d801ec3ecc1088e5'
HERE = Path(__file__).resolve().parent
sha = lambda b: hashlib.sha256(b).hexdigest()


def prepare_card(image, nef, output, raw_only=False):
    sys.path.insert(0, '/path/to/user/fxfix/v52-root/driver-g/retry-driver')
    import card_export
    import exfat_put
    payload = nef.read_bytes()
    assert sha(payload) == NEF_SHA
    catalog = card_export.catalog(image)['files']
    folder = next(x for x in catalog if x['path'] == 'DCIM/100NCZ_8')
    assert folder['contiguous'] and folder['directory']
    assert any(x['path'] == 'DCIM/100NCZ_8/DSC_0001.JPG' for x in catalog)
    assert any(x['path'] == 'LIVE/100NCZ_8/DSC_0001.MOV' for x in catalog)
    assert not any(x['path'].endswith('.NEF') for x in catalog)
    retired = []
    with image.open('r+b') as f:
        part = card_export.volume_offset(image)
        f.seek(part)
        vbr = f.read(512)
        _, _, heap, count, root = struct.unpack_from('<IIIII', vbr, 80)
        ss = 1 << vbr[108]
        sc = ss << vbr[109]
        co = lambda c: part + heap * ss + (c - 2) * sc
        f.seek(co(root))
        rd = f.read(sc)
        bitmap_entry = next(rd[i:i+32] for i in range(0, len(rd), 32) if rd[i] == 0x81)
        bm, blen = struct.unpack_from('<IQ', bitmap_entry, 20)
        f.seek(co(bm))
        bitmap = bytearray(f.read(blen))
        f.seek(co(folder['first_cluster']))
        directory = bytearray(f.read(folder['bytes']))
        retire_paths = ['DCIM/100NCZ_8/NC_FLLST.DAT']
        if raw_only:
            retire_paths.append('DCIM/100NCZ_8/DSC_0001.JPG')
        for cache in [x for x in catalog if x['path'] in retire_paths]:
            assert cache['contiguous'] and not cache['directory']
            i = 0
            while directory[i]:
                n = directory[i+1]+1 if directory[i] == 0x85 else 1
                if directory[i] == 0x85 and struct.unpack_from('<I', directory, i+52)[0] == cache['first_cluster']:
                    for j in range(n):
                        directory[i+32*j] &= 0x7f
                i += 32*n
            for k in range((cache['bytes']+sc-1)//sc):
                c = cache['first_cluster']+k
                bitmap[(c-2)//8] &= ~(1 << ((c-2) % 8))
            retired.append(cache['path'])
        end = 0
        while directory[end]:
            end += 32 * (directory[end+1]+1 if directory[end] == 0x85 else 1)
        n = (len(payload)+sc-1)//sc
        cluster = next(c for c in range(2, count+2-n)
                       if all(not bitmap[(c+k-2)//8] & (1 << ((c+k-2) % 8)) for k in range(n)))
        for k in range(n):
            bitmap[(cluster+k-2)//8] |= 1 << ((cluster+k-2) % 8)
        entry = exfat_put._entry_set('DSC_0001.NEF', 0x20, cluster, len(payload))
        assert end+len(entry)+32 <= len(directory)
        directory[end:end+len(entry)] = entry
        f.seek(co(cluster))
        f.write(payload)
        f.seek(co(folder['first_cluster']))
        f.write(directory)
        f.seek(co(bm))
        f.write(bitmap)
        if raw_only:
            man = next(x for x in catalog if x['path'] == 'LIVEDATA/100NCZ_8/DSC_0001.MAN')
            mandir = next(x for x in catalog if x['path'] == 'LIVEDATA/100NCZ_8')
            assert man['contiguous'] and mandir['contiguous']
            f.seek(co(man['first_cluster']))
            original = f.read(man['bytes'])
            rewritten = b'\n'.join(
                b'native=A:\\DCIM\\100NCZ_8\\DSC_0001.NEF' if line.startswith(b'native=')
                else ('size=%d' % len(payload)).encode() if line.startswith(b'size=')
                else line for line in original.split(b'\n'))
            assert len(rewritten) < sc
            f.seek(co(man['first_cluster']))
            f.write(rewritten)
            f.seek(co(mandir['first_cluster']))
            md = bytearray(f.read(mandir['bytes']))
            pos = 0
            while md[pos]:
                rows = md[pos+1]+1 if md[pos] == 0x85 else 1
                if md[pos] == 0x85 and struct.unpack_from('<I', md, pos+52)[0] == man['first_cluster']:
                    replacement = exfat_put._entry_set('DSC_0001.MAN', 0x20, man['first_cluster'], len(rewritten))
                    assert len(replacement) == rows*32
                    md[pos:pos+len(replacement)] = replacement
                    break
                pos += rows*32
            else:
                raise AssertionError('Private MAN entry not found')
            f.seek(co(mandir['first_cluster']))
            f.write(md)
    after = card_export.catalog(image)
    assert any(x['path'] == 'DCIM/100NCZ_8/DSC_0001.NEF' for x in after['files'])
    assert not any(x['path'].endswith('NC_FLLST.DAT') for x in after['files'])
    after.update(added_nef_source=str(nef), added_nef_sha256=NEF_SHA,
                 retired_private_entries=retired, raw_only=raw_only,
                 warning='NEF0128 and existing JPG/MOV/MAN are not from the same exposure. Synthetic same-card DCF group for RAW anchor layout only.')
    (output / 'fixture.json').write_text(json.dumps(after, indent=2) + '\n')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--main', type=Path, required=True)
    ap.add_argument('--pb-manifest', type=Path, required=True)
    ap.add_argument('--layout', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--card-template', type=Path, default=Path('/path/to/user/fxfix/onemov/pom8f-card.raw'))
    ap.add_argument('--nef', type=Path, default=Path('/path/to/Nikon/z8-qemu/.local/firmware/_DSC0128.NEF'))
    ap.add_argument('--qemu', type=Path, default=Path('/path/to/user/fxfix/v53-tx16-counter-rdw-v3/qemu-system-aarch64'))
    ap.add_argument('--seconds', type=int, default=1500)
    ap.add_argument('--virtual-seconds', type=int, default=40)
    ap.add_argument('--raw-only', action='store_true', help='Retire native JPG and synthesize a direct NEF MAN; tests RAW anchor, not peer lookup')
    a = ap.parse_args()
    assert sha(a.main.read_bytes()) == MAIN_SHA
    root = a.output.resolve()
    root.mkdir(parents=True, exist_ok=False)
    card = root / 'private-raw-card.raw'
    subprocess.run(['cp', '--reflink=auto', str(a.card_template.resolve()), str(card)], check=True)
    prepare_card(card, a.nef.resolve(), root, a.raw_only)
    layout = json.loads(a.layout.read_text())
    offsets = layout['offsets']
    symbols = json.loads(a.pb_manifest.read_text())['symbols']
    observe = symbols['pb_nb_observe']
    plugin = root / 'raw-nbobs.so'
    flags = subprocess.check_output(['pkg-config', '--cflags', '--libs', 'glib-2.0'], text=True).split()
    qemu_headers = Path('/path/to/user/.local/share/nikon-z8-qemu/rdw/qemu-11.1.1/include/plugins')
    assert (qemu_headers / 'qemu-plugin.h').is_file()
    command = ['gcc', '-shared', '-fPIC', '-O2', '-I', str(qemu_headers),
               '-DRAW_OBSERVE_PC=%sull' % hex(observe),
               '-DRAW_REQUEST_OFFSET=%u' % offsets['nb_request'],
               '-DRAW_NATIVE_OFFSET=%u' % offsets['native_path'],
               '-DRAW_MANIFEST_OFFSET=%u' % offsets['manifest.native_path'],
               '-DRAW_NODE_OFFSET=%u' % offsets['nb_node'],
               str(HERE / 'raw_nbobs.c'), '-o', str(plugin)] + flags
    subprocess.run(command, check=True)
    runner = root / 'run-nb-raw.py'
    runner_src = Path('/path/to/user/fxfix/onemov/run-nb.py').read_text()
    assert runner_src.count('/path/to/user/fxfix/onemov/nbobs.so') == 1
    runner.write_text(runner_src.replace('/path/to/user/fxfix/onemov/nbobs.so', str(plugin)))
    contacts = root / 'contacts.json'
    contacts.write_text(json.dumps([
        {'at': 20, 'property': 'mcu-contacts', 'value': '23.5=0'},
        {'at': 20.4, 'property': 'mcu-contacts', 'value': '23.5=1'},
        {'at': 29, 'property': 'mcu-contacts', 'value': '22.12=0'},
        {'at': 29.4, 'property': 'mcu-contacts', 'value': '22.12=1'},
    ], indent=2) + '\n')
    points = [observe, 0x406364D0, 0x403AD6A4, 0x403F6FAC, 0x40711328,
              0x4085FCC4, 0x402DD4F8, 0x40CFC820, 0x40CF7D0C,
              0x403F6E88, 0x403F6F10, 0x4063B95C, 0x403F7244, 0x403EC3A0]
    env = dict(os.environ, NB_PEER='1', NB_OUT=str(root / 'trace.jsonl'),
               NB_PTS=':'.join('%x' % p for p in points), Z8_TOLERATE='15431000:15432000')
    command = ['/path/to/user/.local/share/nikon-z8-qemu/venv/bin/python', '-B', '-u',
        str(runner), '--firmware', str(a.main.resolve()), '--qemu', str(a.qemu.resolve()),
        '--raw', '/path/to/Nikon/z8-qemu/.local/input.raw', '--mcu', '/path/to/user/fxfix/exmcu/EX.bi',
        '--output', str(root / 'run'), '--mcu-pins', 'G1=1:G13=1:G14=1:G15=1',
        '--experimental-shared221', '--experimental-boundary-inputs', '--experimental-sensor-source-clock',
        '--experimental-yuv-channels', '--experimental-movie-selector16', '--card', str(card),
        '--experimental-playback', '--experimental-movie-playback', '--movie-audio-output', 'none',
        '--contact-replay', str(contacts), '--seconds', str(a.seconds), '--virtual-seconds', str(a.virtual_seconds)]
    identity = {'main_sha256': MAIN_SHA, 'qemu_sha256': sha(a.qemu.read_bytes()),
                'observer_sha256': sha(plugin.read_bytes()), 'observer_source_sha256': sha((HERE / 'raw_nbobs.c').read_bytes()),
                'runner_source_sha256': sha(Path(__file__).read_bytes()),
                'qemu_plugin_header_sha256': sha((qemu_headers / 'qemu-plugin.h').read_bytes()),
                'layout_sha256': sha(a.layout.read_bytes()), 'card_template': str(a.card_template.resolve()),
                'private_card': str(card), 'nef_sha256': NEF_SHA, 'command': command,
                'raw_only': a.raw_only,
                'scope': 'Single NVMe, synthetic NEF/Live pairing; not same-exposure or dual-card proof. RAW-only mode uses direct NEF MAN and does not exercise JPG/peer resolver.'}
    (root / 'identity.json').write_text(json.dumps(identity, indent=2) + '\n')
    print(json.dumps({'phase': 'running', 'output': str(root), 'main_sha256': MAIN_SHA}), flush=True)
    with (root / 'run.log').open('w') as log:
        status = subprocess.run(command, env=env, cwd='/path/to/Nikon/rdwing-z8-qemu',
                                stdout=log, stderr=subprocess.STDOUT).returncode
    result_path = root / 'run/result.json'
    result = json.loads(result_path.read_text()) if result_path.exists() else {}
    fields = ['nb_stage', 'nb_photo', 'nb_movie', 'nb_starts', 'nb_returns', 'nb_redirects',
              'nb_w', 'nb_h', 'nb_bit', 'nb_fail', 'nb_cleanups']
    observations, pages, settings, selected_events, opened = [], [], [], [], []
    counts = Counter()
    trace_path = root / 'trace.jsonl'
    if trace_path.exists():
        for trace_index, line in enumerate(trace_path.read_text().splitlines()):
            row = json.loads(line)
            pc = int(row['pc'], 16)
            regs = [int(v, 16) for v in row['regs']]
            counts[hex(pc)] += 1
            if pc == observe:
                raw = bytes.fromhex(row.get('x0_mem', ''))
                obs = {'where': regs[1], 'trace_index': trace_index}
                for name in fields:
                    at = offsets[name] - offsets['nb_request']
                    if 0 <= at and at + 4 <= len(raw):
                        obs[name] = int.from_bytes(raw[at:at+4], 'little')
                for name in ['nb_from', 'nb_to']:
                    at = offsets[name] - offsets['nb_request']
                    if 0 <= at < len(raw):
                        obs[name] = raw[at:at+64].split(b'\0')[0].decode(errors='replace')
                for key in ['selected_native', 'manifest_native']:
                    obs[key] = bytes.fromhex(row.get(key, '')).split(b'\0')[0].decode(errors='replace')
                if row.get('dcf_member_mask'):
                    obs['dcf_member_mask'] = int.from_bytes(bytes.fromhex(row['dcf_member_mask']), 'little')
                observations.append(obs)
            elif pc == 0x403AD6A4:
                pages.append(regs[:2])
            elif pc == 0x403EC3A0 and regs[0] == 0x4910:
                settings.append(regs[1])
                selected_events.append({'trace_index': trace_index, 'id': regs[1]})
            elif pc == 0x402DD4F8:
                opened.append(bytes.fromhex(row.get('x0_mem', '')).split(b'\0')[0].decode(errors='replace'))
    entered = [o for o in observations if o['where'] == 2 and o.get('nb_stage') == 2]
    returned = [o for o in observations if o['where'] == 4 and o.get('nb_stage') == 3]
    cleaned = [o for o in observations if o['where'] == 3 and o.get('nb_stage') == 0]
    checks = {
        'runner_exit_zero': status == 0,
        'correct_main_executed': result.get('firmware_sha256') == MAIN_SHA,
        'native_checks_passed': bool(result.get('checks')) and all(x.get('passed') for x in result['checks']),
        'no_runner_error': result.get('error') is None,
        'bridge_entered': bool(entered), 'bridge_returned': bool(returned), 'bridge_cleaned': bool(cleaned),
        'actual_selected_path_is_NEF': bool(entered and entered[-1]['selected_native'].endswith('.NEF')),
        'actual_live_owner_matches_fixture': bool(entered and entered[-1]['manifest_native'].endswith('.NEF' if a.raw_only else '.JPG')),
        'movie_id_derived_from_raw_anchor': bool(entered and entered[-1]['nb_movie'] == ((entered[-1]['nb_photo'] & ~15) | 5)),
        'open_redirected': bool(cleaned and cleaned[-1].get('nb_redirects', 0) > 0),
        'actual_DCF_MOV_bit_present_during_play': bool(entered and entered[-1].get('dcf_member_mask', 0) & 0x20),
        'actual_DCF_MOV_bit_cleared_after_play': bool(cleaned and 'dcf_member_mask' in cleaned[-1] and not (cleaned[-1]['dcf_member_mask'] & 0x20)),
        'original_RAW_id_selected_on_return': bool(entered and returned and any(
            event['trace_index'] > returned[-1]['trace_index'] and event['id'] == entered[-1]['nb_photo']
            for event in selected_events)),
        'video_decode_calls_observed': counts[hex(0x40CFC820)] > 1,
        'NEF_file_opened': any(p.endswith('.NEF') for p in opened),
        'movie_page_observed': [3, 0x56C] in pages, 'photo_page_observed': [3, 0x552] in pages,
    }
    summary = {'identity': identity, 'checks': checks, 'passed': all(checks.values()),
               'native_checks_count': len(result.get('checks', [])),
               'native_failed_checks': [x for x in result.get('checks', []) if not x.get('passed')],
               'observations': observations, 'pages': pages, 'selected_ids': settings, 'selected_events': selected_events,
               'opened_paths': sorted(set(opened)), 'point_counts': dict(counts), 'runner_error': result.get('error')}
    (root / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps({'passed': summary['passed'], 'checks': checks, 'output': str(root)}), flush=True)
    raise SystemExit(0 if summary['passed'] else 1)


if __name__ == '__main__':
    main()
