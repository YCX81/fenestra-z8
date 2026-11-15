#!/usr/bin/env python3
"""Integrate z8-qemu 8357634 on the v49 portrait tables without losing prior fixes.

Only the 548 portrait rectangles and the 124-byte histogram block are written.
The final aggregate firmware builder owns bootguard stamping and packaging.
"""
import argparse
import hashlib
import importlib.util
import json
import struct
from pathlib import Path

BASE = 0x40160000
STOCK_SHA = '2b43a31c9de838f84b12416de4348a7004e65be07f853ec56ec2b10f3a8e3853'
V49_SHA = '63240425765acad54030e58f2dad83b2f4679a9e511a7e5b03e2c4ba1a28718a'
SOURCES = {
    'video_layout_v2.py': 'b9031292e13a84737771ce7a0e02721d822e594ce766100ba7944d6500af9576',
    'histogram_portrait_fix.py': 'f0aa7fc9583846d731745b057ce15f5b671fc388ffab99e8b9e70613e74bc9ac',
}
# Keystone output of the pinned histogram_portrait_fix.py ASM at 0x4041b248.
# Keeping the exact bytes lets the merger run without a host assembler dependency.
HIST_CODE = bytes.fromhex(
    '8b04003403c9fd9740040035098a9fd28965a0f24901c0f222f9443980038e52'
    'c055a87281008052e3031f2a8d40ff97f503002a28cd80d2a865a0f24801c0f2'
    '080140391f09007161010054c0b983520040a0723f44ff9708bc8152e855a872'
    '09ba8152e955a8721f0400712801889a0300001408b88152e855a872')


def sha(data):
    return hashlib.sha256(data).hexdigest()


def need(ok, message):
    if not ok:
        raise ValueError(message)


def plan(stock, reference, repo):
    need(sha(stock) == STOCK_SHA, 'stock C3.11 identity mismatch')
    need(sha(reference) == V49_SHA, 'v49 reference identity mismatch')
    here = repo / 'experiments/20261114/portrait-full'
    for name, expected in SOURCES.items():
        need(sha((here / name).read_bytes()) == expected, f'upstream source drift: {name}')
    spec = importlib.util.spec_from_file_location('v50_upstream_layout', here / 'video_layout_v2.py')
    layout = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(layout)
    patches = []
    rows = []
    for address, fields, landscape, original_portrait in layout.rows(stock):
        if any(fields[8:10]) or not any(fields[6:8]):
            continue
        new = layout.portrait_rect(fields[14], landscape)
        # Both v49 records are equal, including its repaired second-table attrs.
        first = address - BASE
        need(reference[first:first+68] == reference[first+0x1b420:first+0x1b420+68],
             f'v49 tables disagree at {address:#x}')
        for delta in (0, 0x1b420):
            at = address + delta + 32
            before = reference[at-BASE:at-BASE+8]
            after = struct.pack('<4h', *new)
            patches.append(dict(address=hex(at), before=before.hex(), after=after.hex(), kind='rectangle'))
        rows.append(dict(widget_id=hex(fields[14]), address=hex(address),
                         landscape=list(landscape), portrait=list(new)))
    need(len(rows) == 274, 'unexpected widget count')
    start, end = 0x4041b248, 0x4041b2c4
    old = stock[start-BASE:end-BASE]
    need(reference[start-BASE:end-BASE] == old, 'v49 histogram block is not stock')
    need(len(HIST_CODE) == end-start, 'histogram code length mismatch')
    patches.append(dict(address=hex(start), before=old.hex(), after=HIST_CODE.hex(), kind='histogram'))
    return patches, rows


def apply(source, patches):
    image = bytearray(source)
    last = 0
    for patch in sorted(patches, key=lambda p: int(p['address'], 16)):
        offset = int(patch['address'], 16) - BASE
        old, new = bytes.fromhex(patch['before']), bytes.fromhex(patch['after'])
        need(last <= offset and offset+len(old) <= len(source) and len(old) == len(new),
             'overlapping, out-of-bounds or resized patch')
        need(source[offset:offset+len(old)] == old,
             f'input mismatch at {patch["address"]}; do not overwrite an independently changed table')
        image[offset:offset+len(old)] = new
        last = offset+len(old)
    # Reversing only declared edits must restore the complete source byte-for-byte.
    restored = bytearray(image)
    for patch in patches:
        offset = int(patch['address'], 16) - BASE
        old = bytes.fromhex(patch['before'])
        restored[offset:offset+len(old)] = old
    need(restored == source, 'unexpected change outside planned spans')
    return bytes(image)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--stock', type=Path, required=True)
    p.add_argument('--v49-reference', type=Path, required=True)
    p.add_argument('--repo', type=Path, required=True)
    p.add_argument('--input', type=Path, required=True)
    p.add_argument('--input-sha', required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    source = a.input.read_bytes()
    need(sha(source) == a.input_sha, 'aggregate input identity mismatch')
    stock, reference = a.stock.read_bytes(), a.v49_reference.read_bytes()
    need(len(source) == len(reference), 'MAIN length mismatch')
    patches, rows = plan(stock, reference, a.repo)
    result = apply(source, patches)
    a.output.parent.mkdir(parents=True, exist_ok=True)
    a.output.write_bytes(result)
    report = dict(upstream_commit='8357634600b1a31a9a74fc0c6d0db704a37a9f9b',
                  input_sha256=sha(source), output_sha256=sha(result), stock_sha256=STOCK_SHA,
                  reference_sha256=V49_SHA, source_sha256=SOURCES, bytes=len(result),
                  changed_bytes=sum(x != y for x, y in zip(source, result)),
                  records_per_table=len(rows), tables=2, writes=len(patches),
                  build_id_restamped=False, physical_camera_verified=False,
                  patches=patches, coordinates=rows)
    a.output.with_suffix(a.output.suffix+'.portrait.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps({k: v for k, v in report.items() if k not in ('patches', 'coordinates')}, indent=2))


if __name__ == '__main__':
    main()
