#!/usr/bin/env python3
"""Check v33's card-write path: put one complete test record in live_w5.diag (stage 99); Live's worker should write LIVEW5A/B."""
import struct, sys, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import livectl
import z8_audio_probe as zp
OFF_N, OFF_D = 0x101380, 0x1013b0
t, p = livectl.connect()
def rd(a, n): return p.debug.read(a, n)
def st(H):
    n, ticks, saved, ab, err, pad = struct.unpack('<6I', rd(H + OFF_N, 24)); return dict(n=n, ticks=ticks, saved=saved, ab=ab, err=err, pad=pad)
try:
    eng = livectl.engine(p); H = struct.unpack('<Q', rd(eng + livectl.O_HW, 8))[0]
    magic = struct.unpack('<I', rd(H, 4))[0]; print(hex(H), hex(magic), st(H), zp.w5_summary(p)['photo_movie'])
    if magic != 0x57354c31: raise SystemExit('Live encoder not up')
    head = rd(H + OFF_D, 64); print('diag head', head.hex())
    s = st(H)
    if s['n'] or s['saved'] or s['pad'] or any(head): raise SystemExit('offsets or state not as expected: nothing written')
    p.debug.write(H + OFF_D + 4, struct.pack('<IQ', 99, 0x1234))
    p.debug.write(H + OFF_D, struct.pack('<I', 1))
    for k in range(10):
        time.sleep(1); s = st(H); print(k, s)
        if s['saved'] or s['err']: break
finally:
    t.__exit__(None, None, None)
