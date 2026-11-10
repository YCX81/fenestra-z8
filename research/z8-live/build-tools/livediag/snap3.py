#!/usr/bin/env python3
"""Read-only time-point snapshot of Live's memory and the class 0x37 managers (RAM only, no writes or native calls).
Usage: snap3.py LABEL. Live's 0x24 allocations are found from the allocator records; each is sampled 4 KiB per 256 KiB."""
import sys, struct, hashlib, json
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import livectl
import z8_audio_probe as zp
out = Path('snap3'); out.mkdir(exist_ok=True); label = sys.argv[1]
t, p = livectl.connect()
def mapped(a, n): return not any(zp.busy_retry(p, p.debug.translate, g)['fault'] for g in range(a & ~0xfff, a + n, 0x1000))
def rd(a, n):
    d = bytearray()
    while len(d) < n: d += zp.busy_retry(p, p.debug.read, a + len(d), min(1008, n - len(d)))
    return bytes(d)
res = {}
def grab(name, a, n):
    if not mapped(a, n): res[name] = 'unmapped'; print(name, hex(a), 'unmapped'); return None
    d = rd(a, n); (out / f'{label}-{name}.bin').write_bytes(d); res[name] = [hex(a), n, hashlib.sha256(d).hexdigest()[:16]]; return d
try:
    S = zp.w5_summary(p); print(S); res['summary'] = S
    eng = livectl.engine(p); H = struct.unpack('<Q', rd(eng + livectl.O_HW, 8))[0]; res['H'] = hex(H)
    grab('h', H, 0xc0)
    P = 0xa02f3e2c0 + 0x24 * 0x5ec8; h = grab('p24', P, 0x5ec8)
    count, meta = struct.unpack_from('<I', h, 0x5e28)[0], struct.unpack_from('<Q', h, 0x5e30)[0]
    r = grab('p24rec', meta, count * 0x70); segs = []
    base, unit = struct.unpack_from('<Q', h, 0)[0], struct.unpack_from('<I', h, 12)[0]
    for i in range(count):
        addr, ln, desc = struct.unpack_from('<QIxxxxQ', r, i * 0x70)
        if desc and ln: segs.append((addr, ln, desc))
    res['segs'] = [[hex(a), hex(l), hex(d)] for a, l, d in segs]; print('segments', res['segs'])
    for a, l, d in segs:
        if not (0x840000000 <= a < 0x900000000) or l > 0x8000000: continue   # skip the big free run
        for off in range(0, l, 0x40000):
            grab(f's{a:x}-{off:08x}', a + off, 0x1000)
    q = 0xa02f3e2c0 + 0x37 * 0x5ec8; grab('p37', q, 0x5ec8)
    for n, a, s in (('f37', 0xa03f31b85, 1), ('h37', 0xa03f31bc8, 4), ('M', 0xa031900f8, 0x70), ('cnt', 0xa03b717f8, 4), ('nodes', 0xa03b6b8d8, 0x80),
                    ('bk37', 0xa03f31878, 8), ('gate', 0xa03f31a68, 0xc), ('pend', 0xa03f31b4c, 1)): grab(n, a, s)
    M = rd(0xa031900f8, 0x70) if mapped(0xa031900f8, 0x70) else b''
    if M:
        for i in range(10):
            o = struct.unpack_from('<Q', M, 8 + 8 * i)[0]
            if o and mapped(o, 0x20): grab(f'M{i}', o, 0x20); print('M', i, hex(o), rd(o, 0x20).hex())
    Tp = rd(0xa03f316b8, 8) if mapped(0xa03f316b8, 8) else None
    if Tp: grab('T', struct.unpack('<Q', Tp)[0], 0x1d4)
    (out / f'{label}.json').write_text(json.dumps(res, indent=1)); print('done', len(res), 'items')
finally:
    t.__exit__(None, None, None)
