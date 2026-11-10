#!/usr/bin/env python3
"""Read-only: v27+ Wave5 close counters of Live's encoder plus the service state."""
import struct, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import livectl
import z8_audio_probe as zp
t, p = livectl.connect()
try:
    eng = livectl.engine(p)
    hw = struct.unpack('<Q', p.debug.read(eng + livectl.O_HW, 8))[0]
    h = struct.unpack('<12I', p.debug.read(hw, 48))
    print(f'live_w5 {hw:#x} magic {h[0]:#x} stream {h[1]} frames {h[7]} busy {h[8]} release_req {h[9]} preempts {h[10]} busy_drops {h[11]}')
    pu, rb, closes, eok, emiss, fin, lcu = struct.unpack('<Q6I', p.debug.read(hw + 0x101358, 32))
    print(f'preempt_us {pu} reopen_blocked {rb} closes {closes} eos_ok {eok} eos_miss {emiss} fin_rc {fin:#x} last_close_us {lcu}')
    print(zp.w5_summary(p))
finally:
    t.__exit__(None, None, None)
