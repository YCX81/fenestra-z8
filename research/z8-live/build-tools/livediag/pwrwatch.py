"""Read-only watcher for the power switch with USB attached: every ~2 s, try to connect and read the
Live UI / Fenestra state; print a line whenever reachability or a value changes.  python pwrwatch.py [SECONDS]"""
import struct, sys, time, io, contextlib
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent)); import livectl
secs = float(sys.argv[1]) if len(sys.argv) > 1 else 90
t0 = time.monotonic(); last = None
while time.monotonic() - t0 < secs:
    row = 'unreachable'
    try:
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            t, p = livectl.connect()
        try:
            q = lambda a: struct.unpack('<Q', p.debug.read(a, 8))[0]
            root = q(0xa032b50c8 + 16); core = q(root + 663 * 96 + 80); reg = q(core + 2528); ui = q(reg + 8)
            sd = struct.unpack('<I', p.debug.read(ui + 0x18, 4))[0]
            usb, ev, kept = struct.unpack('<3I', p.debug.read(ui + 0x4828, 12))
            act, ph, err, csd, _ = struct.unpack('<5I', p.debug.read(core + 72, 20))
            eng = q(ui + 8); poll = struct.unpack('<I', p.debug.read(eng + 20, 4))[0]
            row = f'ui_shutdown={sd} usb={usb} usb_events={ev} standby_kept={kept} fx act={act} phase={ph} err={err} shutdown={csd}'
        finally:
            t.__exit__(None, None, None)
    except Exception as e:
        row = 'unreachable' if 'WPD' in str(e) else f'error {type(e).__name__}: {str(e)[:60]}'
    if row != last:
        print(f'{time.monotonic() - t0:6.1f}s {row}', flush=True); last = row
    time.sleep(2)
