#!/usr/bin/env python3
"""离线补丁：C10..C64 的共享描述符 0x21f -> 0x229；不覆盖已有输出。"""
import argparse
import hashlib
import json
import struct
from pathlib import Path

BASE = 0x40160000
REFERENCE_SHA256 = 'c7c6dbeea8a94064a10d47a7b9022ab26e4aa6b12ba5fb63b1ad85abac4fe4d4'
RAM_BASE = 0xa1162b000
FIRST_STYLE = 0x40b37214
COUNT = 275
PROTECTED = [
    ('bootguard cave (conservative)', 0x401fe000, 0x40200000),
    ('Live payload envelope', 0x401e2000, 0x401fff80),
    ('NFP lookup/remap/private descriptors envelope', 0x40a41000, 0x40a42000),
] + [('bootguard hook', a, a + 4) for a in (
    0x401c4d4c, 0x401c4d58, 0x401c4d7c, 0x401cf98c, 0x401d8960, 0x401d8984)]


def sha(b):
    return hashlib.sha256(b).hexdigest()


def spans(data):
    p = 0x40b20200 - BASE
    entries = []
    while True:
        dest, size = struct.unpack_from('<II', data, p)
        p += 8
        if not size:
            break
        if size % 8 or dest + size > 0x100000 or len(entries) >= 128:
            raise ValueError('Invalid boot span table')
        entries.append((dest, size))
    if p != 0x40b20608 - BASE:
        raise ValueError('Boot span table layout changed')
    result = []
    for dest, size in entries:
        result.append((dest, size, p))
        p += size
    if p != 0x40b39460 - BASE:
        raise ValueError('Boot data size changed')
    return result


def patch(data, ref):
    if sha(ref) != REFERENCE_SHA256:
        raise ValueError('bg34.main reference SHA-256 mismatch')
    if len(data) != len(ref):
        raise ValueError('Input MAIN size differs from bg34.main')
    mapping = spans(ref)
    # Guard the mapping as well as every descriptor/string; allow unrelated patches.
    if data[0x40b20000-BASE:0x40b20608-BASE] != ref[0x40b20000-BASE:0x40b20608-BASE]:
        raise ValueError('Boot copy code/table differs from bg34.main')

    def source(ram, size):
        off = ram - RAM_BASE
        hits = [p + off - dest for dest, n, p in mapping if dest <= off and off + size <= dest + n]
        if len(hits) != 1:
            raise ValueError(f'No unique boot mapping for {ram:#x}')
        return hits[0]

    changes = []
    for i in range(COUNT):
        ram = 0xa116d0200 + i * 32
        p = source(ram, 16)
        ptr, length, style = struct.unpack_from('<QII', ref, p)
        if (ptr, length, style) != (0xa116cf000 + i * 16, 4, 0x21f):
            raise ValueError('Unexpected reference descriptor')
        s = source(ptr, 5)
        if ref[s:s+5] != f'C-{10+i//5}\0'.encode():
            raise ValueError('Unexpected reference label')
        if data[p:p+16] != ref[p:p+16] or data[s:s+5] != ref[s:s+5]:
            raise ValueError(f'Target descriptor/string differs from bg34.main: ID {0x7e00+i:#x}')
        pos = p + 12
        if pos + BASE != FIRST_STYLE + i * 32:
            raise ValueError('Unexpected style file address')
        changes.append(dict(id=hex(0x7e00+i), slot=10+i//5, variant=i%5,
                            offset=hex(pos), address=hex(pos+BASE), ram_address=hex(ram+12),
                            old='1f', new='29'))
    out = bytearray(data)
    for c in changes:
        out[int(c['offset'], 16)] = 0x29
    checks = []
    for name, a, b in PROTECTED:
        if any(a <= int(c['address'], 16) < b for c in changes):
            raise ValueError('Protected region overlap')
        checks.append(dict(name=name, start=hex(a), end_exclusive=hex(b),
                           byte_identical=data[a-BASE:b-BASE] == out[a-BASE:b-BASE],
                           sha256=sha(data[a-BASE:b-BASE])))
    # Revert only the allowlisted bytes and compare the entire MAIN.
    restored = bytearray(out)
    for c in changes:
        restored[int(c['offset'], 16)] = 0x1f
    if restored != data:
        raise ValueError('Unexpected change outside allowlist')
    report = dict(schema=1, reference_sha256=sha(ref), input_sha256=sha(data),
                  output_sha256=sha(out), bytes=len(out), changed_bytes=len(changes),
                  changed=[dict(start=c['address'], end_exclusive=hex(int(c['address'],16)+1),
                                offset=c['offset'], old=c['old'], new=c['new']) for c in changes],
                  byte_changes=changes, protected=checks,
                  outside_changed_bytes_identical=True,
                  scope='All consumers of shared IDs 0x7e00..0x7f12; see C64LABEL-RESULT.md',
                  qemu_run=False, camera_access=False)
    return out, report


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('input', type=Path)
    p.add_argument('output', type=Path)
    p.add_argument('--reference', type=Path, default=Path(__file__).with_name('bg34.main'))
    p.add_argument('--json', type=Path)
    a = p.parse_args()
    report_path = a.json or Path(str(a.output) + '.changed.json')
    paths = [a.input.resolve(), a.reference.resolve()]
    if a.output.resolve() in paths or report_path.resolve() in paths or a.output.resolve() == report_path.resolve():
        p.error('Input/reference/output/JSON paths must not alias')
    if a.output.exists() or report_path.exists():
        p.error('Refusing to overwrite existing output')
    try:
        out, report = patch(a.input.read_bytes(), a.reference.read_bytes())
    except (ValueError, OSError, struct.error) as e:
        p.error(str(e))
    with a.output.open('xb') as f:
        f.write(out)
    with report_path.open('x') as f:
        json.dump(report, f, indent=2)
        f.write('\n')
    print(f"SHA256 {report['output_sha256']}  {a.output}")
    print(f"{COUNT} bytes, {COUNT} disjoint one-byte ranges; report: {report_path}")
    for c in report['byte_changes']:
        print(f"{c['address']} (offset {c['offset']}, RAM {c['ram_address']}, ID {c['id']}): 1f -> 29")


if __name__ == '__main__':
    main()
