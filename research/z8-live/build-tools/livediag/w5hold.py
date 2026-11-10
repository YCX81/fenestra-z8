#!/usr/bin/env python3
"""Arm the 4 s experiment on v33: set close_lock=1 (H+0x101378) on a healthy fresh stream, so the hook's w5_close waits 4 s after stage 2."""
import struct, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import livectl
import z8_audio_probe as zp
t, p = livectl.connect()
try:
    eng = livectl.engine(p); H = struct.unpack('<Q', p.debug.read(eng + livectl.O_HW, 8))[0]
    magic = struct.unpack('<I', p.debug.read(H, 4))[0]; S = zp.w5_summary(p)
    lock, failed = struct.unpack('<II', p.debug.read(H + 0x101378, 8)); n, _, saved = struct.unpack('<III', p.debug.read(H + 0x101380, 12))
    print(hex(H), hex(magic), 'lock', lock, 'failed', failed, 'diag', n, saved, S)
    if magic != 0x57354c31 or lock or failed or n or S['client_map'][8:10] != '00' or S['photo_movie'] != 0: raise SystemExit('not a healthy fresh stream in photo mode: nothing written')
    p.debug.write(H + 0x101378, struct.pack('<I', 1))
    print('close_lock now', struct.unpack('<I', p.debug.read(H + 0x101378, 4))[0])
finally:
    t.__exit__(None, None, None)
