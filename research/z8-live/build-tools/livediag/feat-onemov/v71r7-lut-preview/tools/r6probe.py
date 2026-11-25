#!/usr/bin/env python3
"""Read-only r6 LV probe: Layer0 v12 metrics + XM6 + filmsim Live counters, N snapshots."""
import json, struct, sys, time
from pathlib import Path
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import livectl
import probe_z8_r5_live as r5

FS_MAGIC, FS_SIZE = 0x46535231, 9528
OFF = dict(live_applied=208, live_skipped=212, last_live_stages=216, live_ws_ready=188, lv_cancel=236,
           lv_submitted=296, lv_busy_skips=304, pool_suspended=4096, lv_tb_state=4100, lcd_shared=9448, dormant=20)

def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 7
    gap = float(sys.argv[2]) if len(sys.argv) > 2 else 2.0
    t, p = livectl.connect()
    try:
        def rm(a, k):
            if not (0xA00000000 <= a <= 0xA40000000 - k) and not (0x40000000 <= a < 0x50000000):
                raise ValueError(hex(a))
            return p.debug.read_range(a, k) if k > 1000 else p.debug.read(a, k)
        q = lambda a: struct.unpack('<Q', rm(a, 8))[0]
        def fsctx():
            root = q(0xa032b50c8 + 16); core = q(root + 663 * 96 + 80); reg = q(core + 2528); ui = q(reg + 8)
            if struct.unpack('<I', rm(ui, 4))[0] != 0x4c50554b: return 0
            c = q(ui + 38424)
            if not c: return 0
            mg, sz = struct.unpack('<II', rm(c, 8))
            return c if (mg == FS_MAGIC and sz == FS_SIZE) else -sz
        c = fsctx()
        snaps = []
        for i in range(n):
            s = r5.take_snapshot(rm)
            s['mono'] = time.monotonic()
            if c and c > 0:
                b = rm(c, 312)
                u32 = lambda o: struct.unpack_from('<I', b, o)[0]
                u64 = lambda o: struct.unpack_from('<Q', b, o)[0]
                sh = rm(c + OFF['lcd_shared'], 64)
                s['fs'] = dict(live_applied=u32(208), live_skipped=u32(212), last_live_stages=u32(216),
                               live_ws_ready=u32(188), lv_cancel=u32(236), dormant=u32(20),
                               lv_submitted=u64(296), lv_busy_skips=u64(304),
                               pool_suspended=struct.unpack('<I', rm(c + 4096, 4))[0],
                               sh_seq=struct.unpack_from('<I', sh, 4)[0],
                               sh_capture_us=struct.unpack_from('<Q', sh, 8)[0],
                               sh_publish_us=struct.unpack_from('<Q', sh, 16)[0],
                               sh_lut=struct.unpack_from('<I', sh, 52)[0], sh_stages=struct.unpack_from('<I', sh, 56)[0],
                               sh_hdf=sh[60], sh_valid=sh[61])
            else:
                s['fs'] = {'ctx': c}
            snaps.append(s)
            if i + 1 < n: time.sleep(gap)
    finally:
        try: t.__exit__(None, None, None)
        except Exception: pass
    rows = []
    for a, b in zip(snaps, snaps[1:]):
        dt = b['mono'] - a['mono']
        la, lb = a.get('layer0') or {}, b.get('layer0') or {}
        fa, fb = a.get('fs') or {}, b.get('fs') or {}
        xa, xb = a.get('xm6_bridge') or {}, b.get('xm6_bridge') or {}
        d = lambda x, y, k: (y.get(k, 0) - x.get(k, 0)) / dt if (k in x and k in y) else None
        r = {'dt': round(dt, 2), 'route': 'EVF' if b.get('panel_bank') == 1 else 'LCD'}
        for k in ('enqueue_seen', 'enqueue_copied', 'grades', 'distinct_sources', 'disp_calls', 'disp_substituted',
                  'disp_kept_current', 'disp_stock_pass', 'live_reuse_hits', 'live_reuse_misses', 'wait_no_source',
                  'wait_no_bank', 'jpeg_pauses', 'grade_timeouts', 'step_bursts', 'errors', 'pool_lock_waits'):
            v = d(la, lb, k); r[k + '/s'] = None if v is None else round(v, 1)
        for k in ('live_applied', 'live_skipped', 'lv_submitted', 'lv_busy_skips', 'sh_seq'):
            v = d(fa, fb, k); r['fs_' + k + '/s'] = None if v is None else round(v, 1)
        for k in ('live_ok', 'fallback_cnt'):
            v = d(xa, xb, k); r['xm6_' + k + '/s'] = None if v is None else round(v, 1)
        for k in ('fps_milli', 'frame_us', 'last_copy_us', 'last_lut_us', 'last_hdf_us', 'last_grain_us', 'last_clean_us',
                  'last_latency_us', 'last_interframe_ms', 'last_sleep_us', 'worker_cores', 'smp_helpers_online',
                  'active_route', 'source_lag', 'neon_active', 'last_result'):
            r[k] = lb.get(k)
        r['xm6_last_live_us'] = xb.get('last_live_us'); r['xm6_last_err'] = xb.get('last_err')
        r['fs_last_live_stages'] = fb.get('last_live_stages'); r['fs_sh_stages'] = fb.get('sh_stages'); r['fs_sh_hdf'] = fb.get('sh_hdf')
        r['live_age_ms'] = None
        rows.append(r)
    out = {'build_id': snaps[-1].get('build_id'), 'fs_ctx': hex(c) if c and c > 0 else c, 'rows': rows, 'snaps': snaps}
    f = HERE / 'logs' / time.strftime('r6probe-%H%M%S.json')
    f.write_text(json.dumps(out, indent=1, default=str))
    print(f)
    print(json.dumps({'build_id': out['build_id'], 'fs_ctx': out['fs_ctx'], 'anchor': snaps[-1].get('anchor'), 'rows': rows}, indent=1, default=str))

main()
