#!/usr/bin/env python3
"""Independent full-file diff and refusal-path checks, no emulation."""
import hashlib
import json
from pathlib import Path
from c64label_patch import patch, BASE

r = Path('bg34.main').read_bytes()
p = Path('bg34.c64label28.main').read_bytes()
assert len(r) == len(p) == 81807709
actual = [i for i, (a, b) in enumerate(zip(r, p)) if a != b]
expected = [0x9d7214 + 32 * i for i in range(275)]
assert actual == expected
assert all((r[i], p[i]) == (0x1f, 0x29) for i in actual)
for i in range(275):
    o = expected[i]
    assert r[o:o+4] == bytes.fromhex('1f020000')
    assert p[o:o+4] == bytes.fromhex('29020000')
rejections = []
for name, data in [('already patched', p), ('truncated', r[:-1]),
                   ('wrong style high byte', r[:expected[0]+1] + b'\x03' + r[expected[0]+2:]),
                   ('wrong boot map', r[:0x9c0200] + b'\xff' + r[0x9c0201:])]:
    try:
        patch(data, r)
    except ValueError as e:
        rejections.append(dict(case=name, rejected=True, reason=str(e)))
    else:
        raise AssertionError(name)
# Unrelated modifications are preserved, not silently reset to reference.
other = bytearray(r)
other[-1] ^= 1
out, _ = patch(bytes(other), r)
assert out[-1] == other[-1]
assert [i for i, (a, b) in enumerate(zip(other, out)) if a != b] == expected
result = dict(input_sha256=hashlib.sha256(r).hexdigest(), output_sha256=hashlib.sha256(p).hexdigest(),
              bytes=len(p), differing_bytes=len(actual), disjoint_ranges=len(actual),
              first_address=hex(BASE+actual[0]), last_address=hex(BASE+actual[-1]),
              allowlist_exact=True, style_words_verified=275, refusal_checks=rejections,
              unrelated_change_preserved=True, qemu_run=False)
Path('evidence/c64label-verification.json').write_text(json.dumps(result, indent=2)+'\n')
print(json.dumps(result, indent=2))
