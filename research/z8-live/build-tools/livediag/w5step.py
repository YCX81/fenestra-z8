#!/usr/bin/env python3
"""Control experiment (fresh Live stream, photo mode, USB attached so live view is stopped): stop, then one trigger at
a time with 0.5 s observation each, until worker state 5. Finish only in state 5, exit only after state 0.
Aborts without touching the stream if Live is still encoding or the stream is not in its running state."""
import struct, sys, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import livectl
import z8_audio_probe as zp
SUBMIT, TRIGGER, STOP, FINISH, EXIT = 0x40c33e34, 0x40c33fb0, 0x40c34078, 0x40c340e8, 0x402f3084
def u32(p, a): return struct.unpack('<I', p.debug.read(a, 4))[0]
def st(p, s):
    inst = struct.unpack('<Q', p.debug.read(0xa02d32758, 8))[0]
    return dict(worker=u32(p, 0xa02d3274c + 4 * s), inst0=u32(p, inst), inst_state=hex(u32(p, inst + 12)), req=u32(p, 0xa02d31068 + 4 * s),
                latch=u32(p, 0xa02d31070 + 4 * s), vdi=hex(struct.unpack('<Q', p.debug.read(0xa02d3d980, 8))[0]), res=hex(u32(p, 0xa02d32748)))
t, p = livectl.connect()
try:
    eng = livectl.engine(p)
    hw = struct.unpack('<Q', p.debug.read(eng + livectl.O_HW, 8))[0]
    f0 = struct.unpack('<8I', p.debug.read(hw, 32)); time.sleep(1.0); f1 = struct.unpack('<8I', p.debug.read(hw, 32))
    s, frames = f1[1], f1[7]
    print('frames', f0[7], '->', frames, 'stream', s, st(p, s))
    if f0[7] != frames: raise SystemExit('Live is still encoding: not touched')
    if st(p, s)['worker'] != 4: raise SystemExit('stream is not running (worker state != 4): not touched')
    print('stop', hex(zp.native_call(p, STOP, (s,)) & 0xffffffff), st(p, s))
    slot = frames % 4; list_ = hw + 0x310 + 0x800; state5 = False
    for n in range(1, 7):
        if slot == 0:
            print(' submit', hex(zp.native_call(p, SUBMIT, (s, 0, list_)) & 0xffffffff))
        rc = zp.native_call(p, TRIGGER, (s, 0, slot, 0, 0)) & 0xffffffff
        for k in range(5):
            time.sleep(0.1); v = st(p, s)
            if v['worker'] == 5: break
        print(f'trigger #{n} slot {slot} rc {rc:#x} ->', v)
        if v['worker'] == 5: state5 = True; break
        slot = (slot + 1) % 4
    if not state5: raise SystemExit('never reached state 5: finish/exit not called')
    print('finish', hex(zp.native_call(p, FINISH, (s,)) & 0xffffffff), st(p, s))
    if st(p, s)['worker'] != 0: raise SystemExit('worker state not 0 after finish: exit not called')
    zp.native_call(p, EXIT, (4,), timeout_ms=20000); time.sleep(0.5); print('exit', st(p, s)); print(zp.w5_summary(p))
finally:
    t.__exit__(None, None, None)
