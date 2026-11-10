"""Read-only: current channel-2 Y address from the v16 register record (B2 +0x20/+0x24) and 64 bytes at two
points of that buffer in image-bank RAM (not registers)."""
import struct, sys
from pathlib import Path
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE)); import livectl
from v16_patch import LV_REGS
t, p = livectl.connect()
try:
    eng = livectl.engine(p); w = struct.unpack(f'<{len(LV_REGS)}I', p.debug.read(eng + livectl.OFFS['lv_regs'][0], 4 * len(LV_REGS)))
    r = {a: w[i] for i, a in enumerate(LV_REGS)}
    y = r[0x15422220] | (r[0x15422224] << 32); mode = struct.unpack('<I', p.debug.read(0xa03bd062c, 4))[0]
    a = y - 0x400000000 if y >= 0xc00000000 else y
    print(f'mode {mode}  B2 Y {y:#x} -> {a:#x}  stride {r[0x15422238]}  F2 out {r[0x1542322c]}x{r[0x15423230]}  B2+0x48 {r[0x15422248]:#x}')
    if not 0x840000000 <= a < 0x8fff47400 - 0x400000: raise SystemExit('outside the image banks; not read')
    for off in (0, 1920 * 540 + 960):
        b = p.debug.read(a + off, 64); print(f'{a + off:#x}:', ' '.join(f'{x:02x}' for x in b))
finally:
    t.__exit__(None, None, None)
