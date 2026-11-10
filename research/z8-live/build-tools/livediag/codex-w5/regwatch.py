"""Read-only: watch the v16 scaler register record (lv_regs_on must be 1) and print changes.  python regwatch.py SECONDS"""
import struct, sys, time
from pathlib import Path
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE)); import livectl
from v16_patch import LV_REGS
secs = float(sys.argv[1]) if len(sys.argv) > 1 else 60
t, p = livectl.connect()
try:
    eng = livectl.engine(p); off = livectl.OFFS['lv_regs'][0]; prev = None; t0 = time.monotonic()
    irq = livectl.OFFS['capture.irq_seq'][0]
    while time.monotonic() - t0 < secs:
        w = struct.unpack(f'<{len(LV_REGS)}I', p.debug.read(eng + off, 4 * len(LV_REGS)))
        mode = struct.unpack('<I', p.debug.read(0xa03bd062c, 4))[0]; seq = struct.unpack('<I', p.debug.read(eng + irq, 4))[0]
        ts = time.monotonic() - t0
        if prev is None: print(f'{ts:5.1f}s baseline taken (photo/movie word {mode}, irq {seq})')
        else:
            ch = [f'{LV_REGS[i]:#x}: {prev[i]:#x} -> {w[i]:#x}' for i in range(len(w)) if w[i] != prev[i] and not 0x15422020 <= (LV_REGS[i] & ~0x300) <= 0x1542202c]
            if ch: print(f'{ts:5.1f}s mode={mode} irq={seq}  ' + ';  '.join(ch), flush=True)
        prev = w; time.sleep(1)
finally:
    t.__exit__(None, None, None)
