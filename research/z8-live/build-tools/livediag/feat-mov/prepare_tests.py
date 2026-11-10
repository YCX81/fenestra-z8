#!/usr/bin/env python3
"""生成本目录 Live 样本的逐项 MOV 实验件，每个输出都可重新验证。"""
import itertools
import json
import subprocess
import sys
from pathlib import Path

import mov_fix as m


ROOT = Path(__file__).resolve().parent
DONOR = ROOT.parents[2] / '_mac-migration/live-session/live-photo-20261030/pair-u02a/DSC_0002.MOV'
DONOR_SHA = '343ad5b65159ae9a8b23229f70e8bd39db73bc4a9b00119662180906d413f30c'
KEYS = ('niko', 'remove_mebx', 'audio_stco', 'size_from_sps')
CASES = [
    ('00-original', (), '输入', '原始字节副本，保留 Apple 配对'),
    ('01-only-niko', KEYS[:1], '00', 'ftyp[20..23] 加入 niko'),
    ('02-only-remove_mebx', ('remove_mebx',), '00', '删除定时 mebx 轨'),
    ('03-only-audio_stco', ('audio_stco',), '00', '音轨 co64 转 stco'),
    ('04-only-size_from_sps', ('size_from_sps',), '00', '容器尺寸及对应矩阵平移跟随 SPS'),
    ('05-niko-no-mebx', KEYS[:2], '01', '删除定时 mebx 轨'),
    ('06-niko-no-mebx-stco', KEYS[:3], '05', '音轨 co64 转 stco'),
    ('07-all-four', KEYS, '06', '容器尺寸及对应矩阵平移跟随 SPS'),
    ('08-all-four-qemu-ncdt', (*KEYS, 'ncdt_donor'), '07', '加入归档 QEMU MOV 的完整 NCDT'),
    ('09-06-crop-to-container', (*KEYS[:3], 'crop_to_container'), '06', '仅给 SPS 增加底部 6 行裁剪，显示 1024×682'),
]


def save(path, data):
    if not path.exists():
        try:
            m.write_new(path, data)
            return
        except FileExistsError:
            pass  # Another generator published this name first.
    if path.read_bytes() != data:
        raise ValueError(f'既有文件不符，停止以保留现场：{path}')


def check(condition, explanation):
    if not condition:
        raise ValueError(explanation)


def validate_combinations(src):
    check(m.rewrite(src) == src, 'no-op changed file')
    original_video = m.inspect(src)['tracks'][0]
    for values in itertools.product((False, True), repeat=len(KEYS)):
        flags = dict(zip(KEYS, values))
        blob = m.rewrite(src, **flags)
        check(m.rewrite(blob, **flags) == blob, f'not idempotent: {flags}')
        tracks = m.inspect(blob)['tracks']
        dimensions = original_video['sps']['display'] if flags['size_from_sps'] else original_video['avc1_size']
        check((blob[20:24] == b'niko') == flags['niko'], 'ftyp mismatch')
        check(len(tracks) == (2 if flags['remove_mebx'] else 3), 'track count mismatch')
        video = next(t for t in tracks if t['handler'] == 'vide')
        check(video['avc1_size'] == video['tkhd_size'] == dimensions, 'geometry mismatch')
        audio = next(t for t in tracks if t['handler'] == 'soun')
        check(audio['offset_type'] == ('stco' if flags['audio_stco'] else 'co64'), 'audio offsets mismatch')
    for malformed in (src[:-1], b'\x00\x00\x00\x07ftyp'):
        try:
            m.inspect(malformed)
        except (ValueError, StopIteration):
            continue
        raise ValueError('malformed MOV accepted')


def validate_transplant(original, candidate, donor, shell):
    expected = m.donor_ncdt(donor)
    check(m.donor_ncdt(candidate) == expected, 'NCDT differs from donor')
    roots = m.parse(candidate)
    moov = m.direct_child(roots, b'moov')
    moov.children.remove(m.direct_child(moov.children, b'udta'))
    check(b''.join(box.encode() for box in roots) == original, 'more than udta was changed versus 07')
    check(m.rewrite(candidate, ncdt_donor=donor) == candidate, 'repeat transplant changed candidate')
    donor_roots = m.parse(donor)
    donor_moov = m.direct_child(donor_roots, b'moov')
    udta = m.direct_child(donor_moov.children, b'udta')
    udta.data = m.direct_child(m.parse(shell), b'udta').data
    incomplete = b''.join(box.encode() for box in donor_roots)
    try:
        m.donor_ncdt(incomplete)
    except ValueError:
        return
    raise ValueError('accepted the empty structure probe as real metadata')


def structure_probe():
    def atom(kind, payload):
        return m.Box(kind, payload).encode()

    header = atom(b'NCHD', b'Nikon\0\x01\0\0\0')
    empty = b''.join(atom(kind, b'') for kind in (b'NCTG', b'NCTH', b'NCVW', b'NCDB'))
    return atom(b'udta', atom(b'NCDT', header + empty))


def validate_crop(original, candidate):
    original_moov = m.direct_child(m.parse(original), b'moov')
    roots = m.parse(candidate)
    new_avcc = m.direct_child(roots, b'moov').one(b'avcC')
    original_avcc = original_moov.one(b'avcC')
    for avcc, old in [(original_avcc.data, True), (new_avcc.data, False)]:
        geom, raw, pos, has_crop, progressive = m.sps_fields(avcc)
        check(progressive and bool(has_crop) != old, 'unexpected SPS format')
        bits = m.Bits(raw)
        bits.i = pos + 1
        if has_crop:
            check([bits.ue() for _ in range(4)] == [0, 0, 0, 3], 'not 6 rows of bottom crop')
        unchanged_bits = ''.join(f'{v:08b}' for v in raw).rstrip('0')
        suffix = avcc[8 + int.from_bytes(avcc[6:8], 'big'):]
        unchanged = unchanged_bits[:pos], unchanged_bits[bits.i:], avcc[:6], avcc[8], suffix
        if old:
            reference = unchanged
        else:
            check(unchanged == reference, 'SPS outside crop, PPS or avcC fields changed')
            check(geom['display'] == [1024, 682], 'incorrect active dimensions')
    new_avcc.data = original_avcc.data
    check(b''.join(box.encode() for box in roots) == original, 'change outside avcC')
    check(m.rewrite(candidate, crop_to_container=True) == candidate, 'crop not idempotent')
    try:
        m.crop_avcc(original_avcc.data, 1280, 852)
    except ValueError:
        return
    raise ValueError('accepted enlargement by SPS crop')


def main():
    out = ROOT / 'tests'
    out.mkdir(exist_ok=True)
    src = (ROOT / 'live-sample.MOV').read_bytes()
    donor = DONOR.read_bytes()
    check(m.sha(donor) == DONOR_SHA, 'unexpected archive donor')
    shell = structure_probe()
    save(out / 'ncdt-structure-only.bin', shell)
    reports = []
    table = ['| 文件 | 对照 | 唯一语义变化 | 字节数 |', '|---|---|---|---:|']
    for name, flags, parent, change in CASES:
        options = {flag: (donor if flag == 'ncdt_donor' else True) for flag in flags}
        blob = m.rewrite(src, **options) if options else src
        path = out / (name + '.MOV')
        save(path, blob)
        report = m.inspect(path.read_bytes())
        report.update(file=path.name, changes=flags, parent=parent)
        reports.append(report)
        table.append(f'| `{path.name}` | {parent} | {change} | {len(blob):,} |')
    validate_combinations(src)
    validate_transplant((out / '07-all-four.MOV').read_bytes(),
                        (out / '08-all-four-qemu-ncdt.MOV').read_bytes(), donor, shell)
    validate_crop((out / '06-niko-no-mebx-stco.MOV').read_bytes(),
                  (out / '09-06-crop-to-container.MOV').read_bytes())
    manifest = {
        'source': {'name': 'live-sample.MOV', 'sha256': m.sha(src)},
        'donor': {'name': str(DONOR.relative_to(ROOT.parents[2])), 'sha256': DONOR_SHA,
                  'ncdt_sha256': m.sha(m.donor_ncdt(donor))},
        'cases': reports,
    }
    save(out / 'current-manifest.json', (json.dumps(manifest, indent=2) + '\n').encode())
    save(out / 'MATRIX.md', ('\n'.join(table) + '\n').encode())
    print('\n'.join(table))
    print('PASS：媒体和盒校验、16 个组合、幂等、07→08 NCDT、06→09 SPS 裁剪。', flush=True)
    subprocess.run([sys.executable, '-B', str(ROOT / 'verify_media.py'), '--if-available'], check=True)


if __name__ == '__main__':
    main()
