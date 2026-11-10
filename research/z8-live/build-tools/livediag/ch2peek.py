"""Read-only: 64 bytes at the start of the current channel-2 frame (image-bank RAM, not registers)."""
import struct, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent)); import livectl
t, p = livectl.connect()
try:
    eng = livectl.engine(p)
    y = struct.unpack('<4Q', p.debug.read(eng + livectl.OFFS['lv_snap'][0], 32))[2]
    a = y - 0x400000000 if y >= 0xc00000000 else y
    if not 0x840000000 <= a < 0x8fff47400 - 0x400000: raise SystemExit(f'ch2 address {y:#x} -> {a:#x} outside the image banks')
    for off in (0, 1920 * 540 + 960):
        b = p.debug.read(a + off, 64); print(f'{a + off:#x}:', ' '.join(f'{x:02x}' for x in b))
finally:
    t.__exit__(None, None, None)
