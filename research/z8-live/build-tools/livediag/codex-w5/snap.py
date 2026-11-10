#!/usr/bin/env python3
"""Read-only RAM snapshot for the Live-never-ran vs Live-ran comparison (ranges from the Astra trace). RAM only,
every page checked with translate first; no native calls, no writes. Usage: snap.py LABEL"""
import sys, time, struct
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import livectl
import z8_audio_probe as zp
R = [(0xa02d30000,0x10000,'svc'),(0xa02d41848,0x130,'instpool'),(0xa02d41a88,0x10,'ucode'),(0xa02e98000,0x3000,'drv'),
     (0xa02ede828,0x10,'staging'),(0xa031b5ec8,0x50,'bufs'),(0xa031b6038,4,'movrs'),(0xa031b6da0,0x18,'movrs_arena'),
     (0xa046135a8,0x280,'res'),(0xa03f31a68,4,'gate'),(0xa02a24d00,0x2c0,'class_map'),(0xa02f3e2c0+0x24*0x5ec8,0x5ec8,'pool24h'),
     (0xa0291c180,0x60,'usb'),(0xa0282c4a8,8,'usbc'),(0xa03228bc0,4,'u1'),(0xa0322816c,4,'u2'),(0xa03228fb0,4,'u3'),(0xa032280c8,8,'view'),(0xa03bd062c,4,'photo')]
label = sys.argv[1]; out = Path('snap'); out.mkdir(exist_ok=True)
t, p = livectl.connect()
try:
    for addr, n, name in R:
        assert not (0x154000000 <= addr < 0x155000000)
        bad = [pg for pg in range(addr & ~0xfff, addr + n, 0x1000) if zp.busy_retry(p, p.debug.translate, pg)['fault']]
        if bad: print(f'{name} {addr:#x}: not mapped, skipped'); continue
        d = bytearray()
        while len(d) < n: d += zp.busy_retry(p, p.debug.read, addr + len(d), min(1008, n - len(d)))
        (out / f'{label}-{name}.bin').write_bytes(d); print(name, hex(addr), n)
    print(zp.w5_summary(p))
finally:
    t.__exit__(None, None, None)
