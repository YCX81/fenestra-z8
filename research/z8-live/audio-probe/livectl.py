#!/usr/bin/env python3
"""Live diagnostics/tuning over Fenestra USB debug for the Live tune build (MAIN 54cc3272, fix v4).
Runs on win-lab next to z8_audio_probe.py. Read-only except `set`, which writes one 32-bit
field of struct live_tune after checking the runtime identity ('LAR1', size 17640) and the tune
magic ('LTN1'), then reads it back.
  python livectl.py state            counters, diag, encoder timing
  python livectl.py log [--last N]   event log (newest last)
  python livectl.py tune             current tune values
  python livectl.py set NAME=VALUE   change one tune value (takes effect on the next frame)"""
import argparse, json, struct, sys, time
from pathlib import Path
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import z8_audio_probe as zp

RT_MAGIC, RT_SIZE = 0x3152414c, 17640
O_DIAG, O_TUNE, O_LOGHEAD, O_LOG, O_HW = 13064, 13552, 13616, 13624, 9096
LOG_N, EV_SIZE = 128, 24
TUNE = ['magic', 'version', 'late_max', 'soft_cancel', 'window_us', 'sample_us', 'stale_us', 'short_start',
        'geometry_reset_us', 'audio', 'ready_fresh_us']
LIMITS = {'late_max': (-1, 8), 'soft_cancel': (0, 1), 'window_us': (300000, 3000000), 'sample_us': (0, 500000),
          'stale_us': (20000, 2000000), 'short_start': (0, 1), 'geometry_reset_us': (100000, 60000000),
          'audio': (0, 1), 'ready_fresh_us': (50000, 3000000)}
EV = {1: 'press', 2: 'accept', 3: 'short_start', 4: 'press_ignored', 5: 'soft_cancel', 6: 'release', 7: 'C0_cancel',
      8: 'pool_cancel', 9: 'geometry', 10: 'alloc_fail', 11: 'freeze', 12: 'freeze_fail', 13: 'mux_begin',
      14: 'mux_end', 15: 'writer', 16: 'ready', 17: 'enc_error', 18: 'mux_abort', 19: 'not_ready'}
OFFS = json.loads((HERE / 'fix4-offsets.json').read_text())
DIAG_U64 = ['steps', 'ineligible', 'new_seq', 'stale', 'interval', 'lease_fail', 'geometry', 'valid1_fail', 'convert_fail',
            'valid2_fail', 'seq_changed', 'cancelled', 'busy', 'ok', 'enc_error', 'last_age_us', 'max_age_us', 'last_lease_us',
            'max_lease_us', 'last_convert_us', 'max_convert_us', 'sum_convert_us', 'n_convert']


def connect():
    log = zp.Log(HERE / 'logs', 'livectl')
    t = zp.open_transport(zp.find_host(None), log)
    t.__enter__()
    from ptp_client import Client
    c = Client(t)
    p = zp.Probe(c, log, 5000)
    p.handshake()
    return t, p


def engine(p):
    q = lambda a: struct.unpack('<Q', p.debug.read(a, 8))[0]
    root = q(0xa032b50c8 + 16); core = q(root + 663 * 96 + 80); reg = q(core + 2528); ui = q(reg + 8); eng = q(ui + 8)
    magic, size = struct.unpack('<II', p.debug.read(eng, 8))
    if magic != RT_MAGIC or size != RT_SIZE:
        raise SystemExit(f'Live runtime {eng:#x}: magic {magic:#x} size {size} — not the tune build (expect size {RT_SIZE})')
    return eng


def tune(p, eng):
    b = p.debug.read(eng + O_TUNE, 4 * len(TUNE))
    v = list(struct.unpack('<II' + 'i' + 'I' * (len(TUNE) - 3), b))
    d = dict(zip(TUNE, v))
    if d['magic'] != 0x314e544c:
        raise SystemExit(f'tune magic {d["magic"]:#x}')
    return d


def state(p, eng):
    b = p.debug.read_range(eng, RT_SIZE)
    out = {}
    for k, (o, n) in OFFS.items():
        if k == 'size': continue
        v = b[o:o + n]
        out[k] = v.split(b'\0')[0].decode('latin1') if n > 8 else int.from_bytes(v, 'little', signed=False)
    D = b[O_DIAG:O_DIAG + 8 + 8 * len(DIAG_U64)]
    diag = dict(zip(DIAG_U64, struct.unpack_from('<%dQ' % len(DIAG_U64), D, 8)))
    if diag['n_convert']: diag['avg_convert_us'] = diag['sum_convert_us'] // diag['n_convert']
    o = O_DIAG + 352  # bufs[8] follow the 352-byte v1 block
    diag['bufs'] = [hex(x) for x in struct.unpack_from('<8Q', b, o) if x]
    nb, _v2, late, _f, _s, ar, ss, gs, _gsince, sc = struct.unpack_from('<II8Q', b, o + 64)
    diag.update(nbufs=nb, accepted_late=late, alloc_retry=ar, short_start=ss, geometry_skip=gs, soft_cancel=sc)
    lo = O_DIAG + 8 + 8 * 27 + 16
    diag.update(dict(zip(['loops', 'last_loop_us', 'max_loop_us', 'last_ui_us', 'max_ui_us', 'last_step_us', 'max_step_us',
                          'last_audio_us', 'max_audio_us'], struct.unpack_from('<9Q', b, lo))))
    out['diag'] = diag
    hw = struct.unpack_from('<Q', b, O_HW)[0]
    if hw:
        try:
            e = dict(zip(['t_in', 't_wait', 't_out', 't_parse', 's_in', 's_wait', 's_out', 's_parse', 'n'],
                         struct.unpack('<9Q', p.debug.read(hw + 0x101310, 72))))
            n = e['n'] or 1
            out['encoder'] = {k: e[k] for k in ('t_in', 't_wait', 't_out', 't_parse', 'n')} | {
                a + '_avg': e['s_' + a] // n for a in ('in', 'wait', 'out', 'parse')}
        except Exception as exc:
            out['encoder'] = repr(exc)
    return out


def events(p, eng, last):
    head = struct.unpack('<I', p.debug.read(eng + O_LOGHEAD, 4))[0]
    raw = p.debug.read_range(eng + O_LOG, LOG_N * EV_SIZE)
    n = min(head, LOG_N); first = head - n
    rows = []
    for i in range(first, head):
        t, ty, a, bb = struct.unpack_from('<QIIQ', raw, (i % LOG_N) * EV_SIZE)
        rows.append((t, ty, a, bb))
    rows = rows[-last:]
    t0 = rows[0][0] if rows else 0
    for t, ty, a, bb in rows:
        extra = ''
        if ty == 9: extra = f' {a >> 16}x{a & 0xffff}'
        print(f'{t / 1e6:10.3f}s +{(t - t0) / 1e3:9.1f}ms  {EV.get(ty, ty):14s} a={a} b={bb}{extra}')
    print(f'{head} events total')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('cmd', choices=['state', 'log', 'tune', 'set'])
    ap.add_argument('arg', nargs='?')
    ap.add_argument('--last', type=int, default=60)
    a = ap.parse_args()
    t, p = connect()
    try:
        eng = engine(p)
        if a.cmd == 'state': print(json.dumps(state(p, eng), indent=1))
        elif a.cmd == 'log': events(p, eng, a.last)
        elif a.cmd == 'tune': print(json.dumps(tune(p, eng), indent=1))
        else:
            name, val = a.arg.split('='); val = int(val, 0)
            if name not in LIMITS: raise SystemExit(f'unknown or fixed field {name}')
            lo, hi = LIMITS[name]
            if not lo <= val <= hi: raise SystemExit(f'{name} must be {lo}..{hi}')
            tune(p, eng)
            addr = eng + O_TUNE + 4 * TUNE.index(name)
            p.debug.write(addr, struct.pack('<i', val))
            print(name, '=', tune(p, eng)[name])
    finally:
        t.__exit__(None, None, None)


if __name__ == '__main__':
    main()
