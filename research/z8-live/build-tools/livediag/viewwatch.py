"""Read-only: watch the native GUI view (0xa032280c8 -> view; id +0x54, state +0xd0, +0x50) and the playback runtime
(playing, frames, restores, detaches, leave requests, render error) every 0.5 s; print a line on change.
python viewwatch.py SECONDS"""
import struct, sys, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent)); import livectl
secs = float(sys.argv[1]) if len(sys.argv) > 1 else 60
t, p = livectl.connect()
try:
    q = lambda a: struct.unpack('<Q', p.debug.read(a, 8))[0]; u = lambda a: struct.unpack('<I', p.debug.read(a, 4))[0]
    root = q(0xa032b50c8 + 16); core = q(root + 663 * 96 + 80); reg = q(core + 2528); pb = q(reg + 16)
    t0 = time.monotonic(); last = None
    while time.monotonic() - t0 < secs:
        v = q(0xa032280c8)
        vid = vst = v50 = -1
        if 0xa00000000 <= v < 0xa40000000:
            raw = p.debug.read(v + 0x50, 8); v50, vid = struct.unpack('<II', raw); vst = u(v + 0xd0)
        a = p.debug.read(pb + 72, 16); frames, playing, _, restores = struct.unpack('<4I', a)
        b = p.debug.read(pb + 8196, 44); rr, leaving, _, presented, detached, err, rendered, leave, _, sdet, _ = struct.unpack('<11I', b)
        row = (f'view {v:#x} id={vid} state={vst} +0x50={v50:#x} | playing={playing} frames={frames} restores={restores} '
               f'presented={presented} rendered={rendered} detached={detached} state_detached={sdet} left={leave} err={err:#x} req={rr} leaving={leaving}')
        key = row
        if key != last: print(f'{time.monotonic() - t0:6.1f}s {row}', flush=True); last = key
        time.sleep(0.5)
finally:
    t.__exit__(None, None, None)
