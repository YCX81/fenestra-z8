#!/usr/bin/env python3
"""Validate the Wave5 hand-back found by disassembly, on a healthy stream, without flashing.
  w5seq.py arm   photo mode, after a real power cycle: all guards, then hold release_req=1 (v29 auto-close off)
  w5seq.py run   movie mode: all guards again, then STOP, TRIGGER k, wait the picture, TRIGGER k+1 (source -1),
                 wait worker 5, FINISH, verify worker 0 and instance freed, EXIT, verify service down
Stops at the first failed check and calls nothing further. Never retries a trigger."""
import struct, sys, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import livectl
import z8_audio_probe as zp
TRIGGER, STOP, FINISH, EXIT = 0x40c33fb0, 0x40c34078, 0x40c340e8, 0x402f3084
INFO = 0xa02d38b98
def u32(p, a): return struct.unpack('<I', p.debug.read(a, 4))[0]
def u64(p, a): return struct.unpack('<Q', p.debug.read(a, 8))[0]
def hdr(p, hw):
    h = struct.unpack('<12I', p.debug.read(hw, 48))
    return dict(magic=h[0], stream=h[1], hw_index=h[5], frames=h[7], busy=h[8], release_req=h[9], preempts=h[10])
def closes(p, hw): return struct.unpack('<Q6I', p.debug.read(hw + 0x101358, 32))[2]
def st(p, s):
    inst = u64(p, 0xa02d32758 + 8 * s)
    return dict(worker=u32(p, 0xa02d3274c + 4 * s), inst=(u32(p, inst), u32(p, inst + 12)), req=u32(p, 0xa02d31068 + 4 * s),
                latch=u32(p, 0xa02d31070 + 4 * s), res=u32(p, 0xa02d32748))
def die(m): raise SystemExit('STOP: ' + m + ' (nothing further called)')
def guards(p, hw, want_hold):
    H = hdr(p, hw); s = H['stream']
    if H['magic'] != 0x57354c31: die('bad magic')
    if s > 1: die('bad stream')
    if closes(p, hw) or H['preempts']: die(f"not a clean boot: closes {closes(p, hw)} preempts {H['preempts']}")
    if not H['frames'] and not H['hw_index']: die('no frame encoded in this boot')
    if H['release_req'] != want_hold: die(f"release_req {H['release_req']}")
    S = zp.w5_summary(p); V = st(p, s)
    if int(S['client_map'][8:10], 16) != s: die('client 4 does not own the stream: ' + S['client_map'])
    if not (S['slot0'] if s == 0 else S['slot1'])[0] or not S['svc_up'] or S['vdi'] != '0x16010000': die(f'service state {S}')
    if V['worker'] != 4 or V['inst'] != (1, 3) or V['req'] or V['latch']: die(f'stream not healthy {V}')
    print('guards ok', H, V, S['photo_movie']); return H, S
cmd = sys.argv[1]
t, p = livectl.connect()
try:
    eng = livectl.engine(p); hw = u64(p, eng + livectl.O_HW)
    if cmd == 'arm':
        H, S = guards(p, hw, 0)
        if S['photo_movie'] != 0: die('not in photo mode')
        p.debug.write(hw + 0x24, struct.pack('<I', 1))
        time.sleep(1.0); a = hdr(p, hw); time.sleep(1.0); b = hdr(p, hw)
        print('held', a, b)
        if b['release_req'] != 1 or b['busy'] or a['frames'] != b['frames']: die('hold not effective')
        print('armed: switch to movie mode, do not press REC')
    else:
        H, S = guards(p, hw, 1)
        if S['photo_movie'] != 1: die('not in movie mode')
        time.sleep(1.0); b = hdr(p, hw)
        if b['frames'] != H['frames'] or b['busy']: die('not quiescent')
        s, k, n = H['stream'], H['frames'] % 4, H['hw_index']
        rc = zp.native_call(p, STOP, (s,)) & 0xffffffff; V = st(p, s); print('stop', hex(rc), V)
        if rc or V['req'] != 1: die('stop')
        rc = zp.native_call(p, TRIGGER, (s, 0, k, 0, 0)) & 0xffffffff; print(f'trigger normal slot {k} rc {rc:#x}')
        if rc: die('normal trigger')
        info = INFO + s * 0x2400 + (n % 9) * 0x480; end = time.monotonic() + 1.0
        while u32(p, info + 0x60) != n * 3000:
            if time.monotonic() > end: die(f'picture {n} did not complete {st(p, s)}')
        V = st(p, s); print('picture done', V)
        if V['latch'] != 1 or V['worker'] != 4: die('state after normal picture')
        k2 = (k + 1) % 4
        rc = zp.native_call(p, TRIGGER, (s, 0, k2, 0, 0)) & 0xffffffff; print(f'trigger drain slot {k2} rc {rc:#x}')
        if rc: die('drain trigger')
        t0 = time.monotonic()
        while (V := st(p, s))['worker'] != 5:
            if time.monotonic() - t0 > 2: die(f'no state 5 {V}')
        print(f'state 5 after {time.monotonic() - t0:.3f} s', V)
        rc = zp.native_call(p, FINISH, (s,)) & 0xffffffff; V = st(p, s); print('finish', hex(rc), V)
        if V['worker'] != 0 or V['inst'][0] != 0: die('finish did not free the instance')
        zp.native_call(p, EXIT, (4,), timeout_ms=20000); time.sleep(0.3)
        V = st(p, s); S = zp.w5_summary(p); print('exit', V, S)
        if S['vdi'] != '0x0' or S['svc_up'] or V['res']: die('service still up')
        p.debug.write(hw + 0x24, struct.pack('<I', 0))
        print('SUCCESS: service down, hold released. Unplug USB and press REC.')
finally:
    t.__exit__(None, None, None)
