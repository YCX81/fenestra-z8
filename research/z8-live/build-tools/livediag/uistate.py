"""Read-only: Live UI state (shutdown flag, USB tracking from v11)."""
import struct, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent)); import livectl
t, p = livectl.connect()
try:
    q = lambda a: struct.unpack('<Q', p.debug.read(a, 8))[0]
    root = q(0xa032b50c8 + 16); core = q(root + 663 * 96 + 80); reg = q(core + 2528); ui = q(reg + 8)
    magic, = struct.unpack('<I', p.debug.read(ui, 4))
    sd = struct.unpack('<4I', p.debug.read(ui + 0x10, 16))
    usb, ev, kept = struct.unpack('<3I', p.debug.read(ui + 0x4828, 12))
    core_sd = struct.unpack('<5I', p.debug.read(core + 64 + 8, 20))
    print(f'ui {ui:#x} magic {magic:#x} command/reload/shutdown/save_error {sd}')
    print(f'usb={usb} usb_events={ev} standby_kept={kept}')
    print('fenestra core activation,phase,error,shutdown,retry =', core_sd)
finally:
    t.__exit__(None, None, None)
