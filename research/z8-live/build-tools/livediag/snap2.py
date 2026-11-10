#!/usr/bin/env python3
"""Read-only follow-up in the same boot as snap.py ran: allocator records, descriptors, movie pools, microcode staging.
RAM only. For each pool the record array (header +0x5e30, 0x70 bytes per unit, count +0x5e28) is read when mapped."""
import sys, struct
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import livectl
import z8_audio_probe as zp
out = Path('snap'); out.mkdir(exist_ok=True)
def grab(p, addr, n, name):
    if any(zp.busy_retry(p, p.debug.translate, pg)['fault'] for pg in range(addr & ~0xfff, addr + n, 0x1000)):
        print(name, hex(addr), 'not mapped'); return None
    d = bytearray()
    while len(d) < n: d += zp.busy_retry(p, p.debug.read, addr + len(d), min(1008, n - len(d)))
    (out / f'ran2-{name}.bin').write_bytes(d); print(name, hex(addr), n); return bytes(d)
t, p = livectl.connect()
try:
    S = zp.w5_summary(p); print(S)
    if S['arena'] != '0x8a2c9b000': raise SystemExit('not the snapshot boot: arena differs')
    grab(p, 0xa1182bb20, 0x4130, 'c24h-records')
    for a, n in ((0xa118a0e28, 'desc135'), (0xa118a5130, 'desc140'), (0xa118abc70, 'desc148')): grab(p, a, 0x18, n)
    for c in (0x3f, 0x46, 0x47, 0x48, 0x3e, 0x3b, 0x3c, 0x3d):
        P = 0xa02f3e2c0 + c * 0x5ec8
        h = grab(p, P, 0x5ec8, f'pool{c:02x}')
        if h:
            count, meta = struct.unpack_from('<I', h, 0x5e28)[0], struct.unpack_from('<Q', h, 0x5e30)[0]
            base, tot, unit = struct.unpack_from('<QII', h, 0)
            print(f' class {c:#x} base {base:#x} total {tot:#x} unit {unit:#x} count {count} meta {meta:#x} type {struct.unpack_from("<I",h,0x14)[0]}')
            if meta and 0 < count <= 4000: grab(p, meta, count * 0x70, f'pool{c:02x}-records')
    grab(p, 0xa17458dc0, 0xa3e80, 'ucode-staging')
    print('done')
finally:
    t.__exit__(None, None, None)
