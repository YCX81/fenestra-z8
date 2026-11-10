#!/usr/bin/env python3
"""Live diagnostics/tuning over Fenestra USB debug for the Live tune builds (v4 MAIN 54cc3272, v5 cached-alias).
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

RT_MAGIC = 0x4c415231
# runtime size -> (field offsets, diag, tune, log head, log)
BUILDS = {17640: ('fix4-offsets.json', 13064, 13552, 13616, 13624),
          17688: ('fix5-offsets.json', 13064, 13552, 13616, 13624),
          17576: ('fix6-offsets.json', 12952, 13440, 13504, 13512),
          18776: ('fix8-offsets.json', 12952, 13440, 13508, 13520),
          18824: ('fix9-offsets.json', 12952, 13440, 13508, 13520),
          18848: ('fix10-offsets.json', 12952, 13440, 13508, 13520),
          18864: ('fix12-offsets.json', 12952, 13440, 13508, 13520),
          20472: ('fix13-offsets.json', 12952, 13440, 13508, 13520),
          20616: ('fix14-offsets.json', 12952, 13440, 13508, 13520),
          21544: ('fix16-offsets.json', 12952, 13440, 13508, 13520),
          21616: ('fix17-offsets.json', 12952, 13440, 13508, 13520),
          23184: ('fix23-offsets.json', 12952, 13440, 13508, 13520)}
O_SNAP, O_SNAPHEAD, O_TRANS = 16640, 17664, 17688   # v8/v9
# v9 flight recorder (struct live_fr, 64 B, ring of 4096)
O_FR_ROWS, O_FR_HEAD, O_FR_ALLOCFAIL, O_FR, O_FR_TORN, O_FR_CHECKED = 17836, 17840, 17844, 17848, 17864, 17872
FR_N, FR_SIZE = 4096, 64
O_FR_DROP, O_DCIM = 17880, 17888   # v10: fr_drop, fr_dropped / dcim_copy, dcim_result, dcim_copies
FR_RESULT = {0: '?', 1: 'kept', 2: 'kept_late', 3: 'stale', 4: 'interval', 5: 'lease_fail', 6: 'alloc_wait', 7: 'geometry',
             8: 'valid1_fail', 9: 'convert_fail', 10: 'cancelled', 11: 'seq_changed', 12: 'valid2_fail', 13: 'encoder_busy', 14: 'torn_drop', 15: 'duplicate'}
RT_SIZE = None
O_DIAG, O_TUNE, O_LOGHEAD, O_LOG, O_HW = None, None, None, None, 9096
LOG_N, EV_SIZE = 128, 24
TUNE = ['magic', 'version', 'late_max', 'soft_cancel', 'window_us', 'sample_us', 'stale_us', 'short_start',
        'geometry_reset_us', 'audio', 'ready_fresh_us', 'cached_lv', 'cached_work', 'cached_slot', 'cached_bs',
        'picker', 'icon_refresh_ms']
TUNE_COUNT = {17640: 11, 17688: 15, 17576: 15, 18776: 17, 18824: 17, 18848: 17, 18864: 17, 20472: 17, 20616: 17, 21544: 17, 21616: 17, 23184: 17}
LIMITS = {'late_max': (-1, 8), 'soft_cancel': (0, 1), 'window_us': (300000, 3000000), 'sample_us': (0, 500000),
          'stale_us': (20000, 2000000), 'short_start': (0, 1), 'geometry_reset_us': (100000, 60000000),
          'audio': (0, 1), 'ready_fresh_us': (50000, 3000000),
          'cached_lv': (0, 1), 'cached_work': (0, 1), 'cached_slot': (0, 1), 'cached_bs': (0, 1),
          'picker': (0, 3), 'icon_refresh_ms': (0, 5000)}
EV = {1: 'press', 2: 'accept', 3: 'short_start', 4: 'press_ignored', 5: 'soft_cancel', 6: 'release', 7: 'C0_cancel',
      8: 'pool_cancel', 9: 'geometry', 10: 'alloc_fail', 11: 'freeze', 12: 'freeze_fail', 13: 'mux_begin',
      14: 'mux_end', 15: 'writer', 16: 'ready', 17: 'enc_error', 18: 'mux_abort', 19: 'not_ready', 20: 'icon_fail',
      21: 'mux_stage', 22: 'dcim_copy', 23: 'ui_event', 24: 'boot_flag', 25: 'mode', 26: 'w5_handback', 27: 'log_save'}
OFFS = None
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
    global RT_SIZE, OFFS, O_DIAG, O_TUNE, O_LOGHEAD, O_LOG
    magic, size = struct.unpack('<II', p.debug.read(eng, 8))
    if magic != RT_MAGIC or size not in BUILDS:
        raise SystemExit(f'Live runtime {eng:#x}: magic {magic:#x} size {size} — not a tune build {sorted(BUILDS)}')
    RT_SIZE = size; f, O_DIAG, O_TUNE, O_LOGHEAD, O_LOG = BUILDS[size]; OFFS = json.loads((HERE / f).read_text())
    return eng


def tune(p, eng):
    names = TUNE[:TUNE_COUNT[RT_SIZE]]
    b = p.debug.read(eng + O_TUNE, 4 * len(names))
    v = list(struct.unpack('<II' + 'i' + 'I' * (len(names) - 3), b))
    return _tune_check(dict(zip(names, v)))


def _tune_check(d):
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
            if RT_SIZE != 17640:
                out['encoder'].update(dict(zip(['flags', 'alias_slot_ok', 'alias_slot_fail', 'alias_bs_ok', 'alias_bs_fail', 'staging_scratch'],
                                               struct.unpack('<6I', p.debug.read(hw + 0x101358, 24)))))
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
        if ty == 21: extra = ' ' + ['jpg copied', 'mov written', 'lvp written', 'man written'][a] + f' at {bb / 1000:.0f} ms'
        print(f'{t / 1e6:10.3f}s +{(t - t0) / 1e3:9.1f}ms  {EV.get(ty, ty):14s} a={a} b={bb}{extra}')
    print(f'{head} events total')


def flight(p, eng, last, csv_path):
    if RT_SIZE not in (18824, 18848, 18864, 20472, 20616, 21544, 21616, 23184): raise SystemExit('flight needs the v9/v10 build')
    rows, head, afail = struct.unpack('<III', p.debug.read(eng + O_FR_ROWS, 12))
    fr, = struct.unpack('<Q', p.debug.read(eng + O_FR, 8))
    torn, checked = struct.unpack('<QQ', p.debug.read(eng + O_FR_TORN, 16))
    print(f'flight ring {fr:#x}: {head} records, tear-check rows {rows}, torn {torn}/{checked} checked, alloc_fail {afail}')
    if RT_SIZE in (18848, 18864, 20472, 20616, 21544, 21616, 23184):
        drop, dropped, dc, dres, dn = struct.unpack('<IIIIQ', p.debug.read(eng + O_FR_DROP, 24))
        print(f'v10: drop torn frames {drop} (dropped {dropped}); DCIM copy {dc} (copies {dn}, last result {dres})')
    if not fr or not head: return
    n = min(head, FR_N, max(last, 1) if not csv_path else FR_N)
    first = head - n; recs = []
    # read only the needed slots, in contiguous runs (the ring may wrap)
    raw, idx = {}, [i % FR_N for i in range(first, head)]
    runs, start = [], 0
    for j in range(1, len(idx) + 1):
        if j == len(idx) or idx[j] != idx[j - 1] + 1: runs.append((idx[start], idx[j - 1])); start = j
    for s0, e in runs:
        b = p.debug.read_range(fr + s0 * FR_SIZE, (e - s0 + 1) * FR_SIZE)
        for k in range(s0, e + 1): raw[k] = b[(k - s0) * FR_SIZE:(k - s0 + 1) * FR_SIZE]
    for i in range(first, head):
        t, seq, after, y, s4, s20, cu, eu, ty, tu, res, pk, nb, _f = struct.unpack('<6Q2I2H4B', raw[i % FR_N])
        recs.append(dict(i=i, t=t, seq=seq, after=after, during=(after - seq) // 2 if after > seq else 0, y=y, s4=s4, s20=s20,
                         conv_us=cu, enc_us=eu, torn_y=ty, torn_uv=tu, result=FR_RESULT.get(res, res), picker=pk, nbufs=nb))
    if csv_path:
        import csv
        with open(csv_path, 'w', newline='') as f:
            w = csv.DictWriter(f, fieldnames=list(recs[0])); w.writeheader()
            for r in recs: w.writerow({k: (hex(v) if k in ('y', 's4', 's20') else v) for k, v in r.items()})
        print('wrote', len(recs), 'records to', csv_path)
    # summary over everything read
    by = {}
    for r in recs:
        k = (r['picker'], r['result']); by[k] = by.get(k, 0) + 1
    print('picker/result counts:', {f'p{a}/{b}': c for (a, b), c in sorted(by.items())})
    for pk in sorted({r['picker'] for r in recs}):
        kept = [r for r in recs if r['picker'] == pk and r['result'] in ('kept', 'kept_late')]
        if not kept: continue
        bad = [r for r in kept if r['torn_y'] or r['torn_uv']]
        hist = [sum(1 for r in bad if r['torn_y'] >> k & 1) for k in range(16)]
        print(f'picker {pk}: kept {len(kept)}, torn {len(bad)} ({100 * len(bad) / len(kept):.1f}%), '
              f'avg convert {sum(r["conv_us"] for r in kept) // len(kept)} us, avg encode {sum(r["enc_us"] for r in kept) // len(kept)} us, '
              f'torn rows (top->bottom) {hist}')
    shown = recs[-last:]; t0 = shown[0]['t'] if shown else 0
    for r in shown:
        mark = ' TORN y=%04x uv=%04x' % (r['torn_y'], r['torn_uv']) if r['torn_y'] or r['torn_uv'] else ''
        print(f"{(r['t'] - t0) / 1e3:9.1f}ms seq {r['seq']:7d} +{r['during']} y={r['y']:#x} s4={r['s4']:#x} p{r['picker']} "
              f"{r['result']:12s} conv {r['conv_us']:5d}us enc {r['enc_us']:6d}us{mark}")


def pb_state(p):
    q = lambda a: struct.unpack('<Q', p.debug.read(a, 8))[0]
    root = q(0xa032b50c8 + 16); core = q(root + 663 * 96 + 80); reg = q(core + 2528); pb = q(reg + 16)
    b = p.debug.read_range(pb, 488)
    magic, size = struct.unpack_from('<II', b, 0)
    u = lambda o: struct.unpack_from('<I', b, o)[0]
    print(json.dumps(dict(pb=hex(pb), magic=hex(magic), bytes=size, key=u(8), media_epoch=u(12), started=u(40), error=u(44),
        polls=u(48), current_id=u(52), cache_valid=u(56), lookup_count=u(64), lookup_error=u(68), frames=u(72), playing=u(76),
        native_path=b[352:416].split(b'\0')[0].decode('latin1'), directory=b[416:480].split(b'\0')[0].decode('latin1'),
        photo_size=struct.unpack_from('<Q', b, 480)[0],
        manifest_head=b[232:352].split(b'\0')[0].decode('latin1', 'replace')), indent=1))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('cmd', choices=['state', 'log', 'tune', 'set', 'pb', 'snap', 'flight', 'rows', 'v10', 'chan', 'rate', 'lvgeo', 'regs', 'ch2', 'views'])
    ap.add_argument('--csv', help='flight: also write all records to this CSV file')
    ap.add_argument('arg', nargs='?')
    ap.add_argument('--last', type=int, default=60)
    a = ap.parse_args()
    t, p = connect()
    try:
        if a.cmd == 'pb':
            pb_state(p); return
        eng = engine(p)
        if a.cmd == 'state': print(json.dumps(state(p, eng), indent=1))
        elif a.cmd == 'log': events(p, eng, a.last)
        elif a.cmd == 'snap':
            if RT_SIZE not in (18776, 18824): raise SystemExit('snap needs the v8/v9 build')
            head, tn = struct.unpack('<II', p.debug.read(eng + O_SNAPHEAD, 8))
            raw = p.debug.read_range(eng + O_SNAP, 16 * 64)
            for i in range(max(0, head - 16), head):
                g, s1, s2, s3, s4, s11, s20, s27 = struct.unpack_from('<8Q', raw, (i % 16) * 64)
                print(f'gen {g:6d} s1={s1:#x} s2={s2:#x} s3={s3:#x} s4={s4:#x} s11={s11:#x} s20={s20:#x} s27={s27:#x}')
            tr = p.debug.read_range(eng + O_TRANS, 8 * 16)
            print('rotation (from -> to):', [f'{a:#x}->{b:#x}' for a, b in struct.iter_unpack('<QQ', tr) if a][:tn])
        elif a.cmd == 'flight': flight(p, eng, a.last, a.csv)
        elif a.cmd == 'rows':
            if RT_SIZE not in (18824, 18848, 18864, 20472, 20616, 21544, 21616, 23184): raise SystemExit('rows needs the v9/v10 build')
            if a.arg is not None:
                v = int(a.arg, 0)
                if not 0 <= v <= 16: raise SystemExit('rows must be 0..16 (0 = tear check off)')
                p.debug.write(eng + O_FR_ROWS, struct.pack('<I', v))
            print('fr_rows =', struct.unpack('<I', p.debug.read(eng + O_FR_ROWS, 4))[0])
        elif a.cmd == 'v10':
            # v10 switches: python livectl.py v10 drop=0|1  /  v10 dcim=0|1
            if RT_SIZE not in (18848, 18864, 20472, 20616, 21544, 21616, 23184): raise SystemExit('v10 switches need the v10+ build')
            if a.arg:
                k, v = a.arg.split('='); v = int(v)
                if k not in ('drop', 'dcim') or v not in (0, 1): raise SystemExit('use drop=0|1 or dcim=0|1')
                p.debug.write(eng + (O_FR_DROP if k == 'drop' else O_DCIM), struct.pack('<I', v))
            drop, dropped, dc, dres, dn = struct.unpack('<IIIIQ', p.debug.read(eng + O_FR_DROP, 24))
            print(f'drop={drop} (dropped {dropped})  dcim={dc} (copies {dn}, last result {dres})')
        elif a.cmd in ('chan', 'rate'):
            # v14: chan N = live-view output channel Live copies (0..3, default 1); rate BPS = encoder bitrate
            if RT_SIZE not in (20616, 21544, 21616, 23184): raise SystemExit('chan/rate need the v14+ build')
            off, lo, hi = (OFFS['lv_channel'][0], 0, 3) if a.cmd == 'chan' else (OFFS['stream.bitrate'][0], 2000000, 40000000)
            if a.arg is not None:
                v = int(a.arg, 0)
                if not lo <= v <= hi: raise SystemExit(f'{a.cmd} must be {lo}..{hi}')
                p.debug.write(eng + off, struct.pack('<I', v))
            print(a.cmd, '=', struct.unpack('<I', p.debug.read(eng + off, 4))[0])
        elif a.cmd == 'lvgeo':
            # v14: geometry of the four live-view outputs as Live itself read it (32-bit loads in the camera).
            # Never read the 0x1542xxxx registers from the host: the debug read is not a 32-bit access and
            # hung the camera twice on 11-07.
            if RT_SIZE not in (20616, 21544, 21616, 23184): raise SystemExit('lvgeo needs the v14+ build')
            raw = p.debug.read(eng + OFFS['lv_geo'][0], 4 * 6 * 4)
            for k in range(4):
                w, h, ys, us, nv, ok = struct.unpack_from('<6I', raw, 24 * k)
                print(f'ch{k}: {w}x{h} stride {ys}/{us} nv12={nv} valid={ok}')
            snap = struct.unpack('<4Q', p.debug.read(eng + OFFS['lv_snap'][0], 32))
            print('snapshot Y per channel:', [hex(x) for x in snap], ' current channel:', struct.unpack('<I', p.debug.read(eng + OFFS['lv_channel'][0], 4))[0])
        elif a.cmd == 'regs':
            # v16: scaler registers as the camera read them (32-bit loads); --csv FILE appends a JSON line
            if RT_SIZE not in (21544, 21616, 23184): raise SystemExit('regs needs the v16+ build')
            if a.arg in ('on', 'off'):
                p.debug.write(eng + OFFS['lv_regs_on'][0], struct.pack('<I', 1 if a.arg == 'on' else 0))
                print('lv_regs_on =', struct.unpack('<I', p.debug.read(eng + OFFS['lv_regs_on'][0], 4))[0]); return
            raw = p.debug.read(eng + OFFS['lv_regs'][0], 912); n = struct.unpack('<I', p.debug.read(eng + OFFS['lv_regs_n'][0], 4))[0]
            w = struct.unpack('<228I', raw)
            sys.path.insert(0, str(HERE)); from v16_patch import LV_REGS
            out = {f'0x{addr:08x}': w[i] for i, addr in enumerate(LV_REGS)}
            print('samples', n)
            for k, v in out.items():
                if v: print(k, f'{v:#010x}')
            if a.csv:
                with open(a.csv, 'a') as f: f.write(json.dumps(dict(t=time.time(), n=n, regs=out)) + '\n')
        elif a.cmd == 'ch2':
            # v17: ch2 on = the camera rewrites scaler channel 2 to 1:1 1280x852 NV12 (photo mode); ch2 off = restore
            if RT_SIZE not in (21616, 23184): raise SystemExit('ch2 needs the v17+ build')
            # v19: a/on = keep stock mode + kick; b = mode-1 variant + kick; off = restore + kick
            if a.arg in ('on', 'a', 'b', 'off'):
                p.debug.write(eng + OFFS['ch2_req'][0], struct.pack('<I', {'on': 1, 'a': 1, 'b': 2, 'off': 0}[a.arg]))
            req, st, ap, rs, ra = struct.unpack('<5I', p.debug.read(eng + OFFS['ch2_req'][0], 20))
            print(f'ch2_req={req} state={st} applies={ap} restores={rs} reapplies={ra}')
        elif a.cmd == 'views':
            # v23: GUI view / playback-module changes recorded in RAM (EV_VIEW 28, EV_PB 29), oldest first
            if RT_SIZE != 23184: raise SystemExit('views needs the v23 build')
            head = struct.unpack('<I', p.debug.read(eng + OFFS['vhead'][0], 4))[0]
            raw = p.debug.read_range(eng + OFFS['vring'][0], 64 * 24)
            ev = [struct.unpack_from('<QIIQ', raw, 24 * (i % 64)) for i in range(max(0, head - 64), head)]
            print('entries', head)
            for t_, ty, a_, b_ in ev:
                if ty == 28: print(f'{t_ / 1e6:9.2f}s VIEW id={a_ >> 8} state={a_ & 255} view={b_:#x}')
                elif ty == 29: print(f'{t_ / 1e6:9.2f}s PB   playing={a_ & 255} presented={(a_ >> 8) & 255} detached={(a_ >> 16) & 255} left={a_ >> 24} err={b_ & 0xffffffff:#x} state_detached={b_ >> 32}')
        elif a.cmd == 'tune': print(json.dumps(tune(p, eng), indent=1))
        else:
            name, val = a.arg.split('='); val = int(val, 0)
            if name not in LIMITS or name not in TUNE[:TUNE_COUNT[RT_SIZE]]: raise SystemExit(f'{name} is not tunable in this build')
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
