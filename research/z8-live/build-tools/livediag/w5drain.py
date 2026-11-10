#!/usr/bin/env python3
"""Experiment (movie mode, Live idle, Live's stream still open): complete Live's open 4-slot batch, then
optionally stock stop/finish, then the stock exit; report whether the service really shut down (VDI 0).
  python w5drain.py [--eos] [--no-exit]"""
import argparse, struct, sys, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import livectl
import z8_audio_probe as zp

TRIGGER, STOP, FINISH, EXIT = 0x40c33fb0, 0x40c34078, 0x40c340e8, 0x402f3084
INFO = 0xa02d38b98
def u32(p, a): return struct.unpack('<I', p.debug.read(a, 4))[0]
def svc(p):
    s = zp.w5_summary(p); w = struct.unpack('<8I', p.debug.read(0xa02d32740, 32))
    inst = struct.unpack('<Q', p.debug.read(0xa02d32758, 8))[0]
    ist = struct.unpack('<4I', p.debug.read(inst, 16)) if inst else ()
    return f"inst {inst:#x} {[hex(x) for x in ist]} vdi {s['vdi']} up {s['svc_up']} slot0 {s['slot0']} busy {s['core_busy']} map {s['client_map']} res {w[2]:#x} wstate {w[3]} eos {u32(p,0xa02d31068)} eosdone {u32(p,0xa02d31070)}"
ap = argparse.ArgumentParser(); ap.add_argument('--eos', action='store_true'); ap.add_argument('--no-exit', action='store_true')
a = ap.parse_args()
t, p = livectl.connect()
try:
    eng = livectl.engine(p)
    hw = struct.unpack('<Q', p.debug.read(eng + livectl.O_HW, 8))[0]
    h = struct.unpack('<8I', p.debug.read(hw, 32))
    magic, stream, frames, hw_index = h[0], h[1], h[7], h[5]
    print(f'live_w5 {hw:#x} magic {magic:#x} stream {stream} frames {frames} hw_index {hw_index} batch_open {h[6]}')
    print('before :', svc(p))
    if a.eos:
        print('stop', hex(zp.native_call(p, STOP, (stream,)) & 0xffffffff))
    todo = list(range(frames % 4, 4)) if frames % 4 else []
    if a.eos and not todo:   # EOS is only processed at a picture completion: run one more full batch
        rc = zp.native_call(p, 0x40c33e34, (stream, 0, hw + 0x310 + 0x800)) & 0xffffffff
        print('submit', hex(rc)); todo = [0, 1, 2, 3]
    for i in todo:
        n = hw_index
        rc = zp.native_call(p, TRIGGER, (stream, 0, i, 0, 0)) & 0xffffffff
        info = INFO + stream * 0x2400 + (n % 9) * 0x480
        t0 = time.monotonic(); ok = False
        while time.monotonic() - t0 < 1.0:
            if struct.unpack('<I', p.debug.read(info + 0x60, 4))[0] == n * 3000: ok = True; break
        print(f'trigger slot {i} rc {rc:#x} picture {n} done {ok}')
        hw_index += 1
        p.debug.write(hw + 20, struct.pack('<I', hw_index))
    p.debug.write(hw + 28, struct.pack('<I', frames + len(todo)))
    print('drained:', svc(p))
    if a.eos:
        t0 = time.monotonic()
        while u32(p, 0xa02d31070 + 4 * stream) == 0 and time.monotonic() - t0 < 2: pass
        print('eos done:', svc(p))
    if a.eos:
        print('finish', hex(zp.native_call(p, FINISH, (stream,)) & 0xffffffff)); print('finished:', svc(p))
    if not a.no_exit:
        zp.native_call(p, EXIT, (4,), timeout_ms=20000); time.sleep(0.5); print('exit   :', svc(p))
finally:
    t.__exit__(None, None, None)
