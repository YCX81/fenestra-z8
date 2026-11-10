#!/usr/bin/env python3
"""Card-lamp identity test: drive GPIO 139 with the stock output function 0x40c15c3c; 3 cycles of 3 s level 1 / 3 s level 0, ends at 0."""
import struct, sys, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import livectl
import z8_audio_probe as zp
D, W = 0xa0280a1b0, 0x40c15c3c
t, p = livectl.connect()
try:
    d = struct.unpack('<4I', p.debug.read(D, 16)); print('descriptor', [hex(x) for x in d])
    if d[0] != 139 or d[1] != 9 or d[2] != 14 or not d[3]: raise SystemExit('descriptor not as expected: nothing written')
    for i in range(3):
        print('level 1'); zp.native_call(p, W, (D, 1)); time.sleep(3)
        print('level 0'); zp.native_call(p, W, (D, 0)); time.sleep(3)
    print('done')
finally:
    t.__exit__(None, None, None)
