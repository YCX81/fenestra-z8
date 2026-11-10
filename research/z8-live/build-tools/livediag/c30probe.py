#!/usr/bin/env python3
"""C30 pre-release check over Fenestra USB Debug, read-only (only debug memory reads).
Samples the JPEG engine statistics (NFP z8precapture: G_JPEG_STATS, 0x240 per engine, rewritten by every
main JPEG encode) and the HSFC mode globals for SECONDS, then prints per-second change counts.
If a half-press in C30 + pre-release makes the camera encode every frame, the stats change ~30x/s
during the half-press and stay still otherwise.
  python c30probe.py [SECONDS]      default 40; raw samples -> logs/c30probe-<time>.csv"""
import struct, sys, time
from pathlib import Path
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import livectl

G = {'G_MODE (6=C15,3=C30)': 0xa03bc6bc8, 'G_LIMIT (pre-count)': 0xa03bc6bac, 'G_RELMODE (3=C30)': 0xa03d7c7a4,
     'G_DRAINED': 0xa03bc5638, 'G_QTYPE': 0xa03d482c8, 'G_PHOTO': 0xa03d74254, 'G_JPEG_FLAGS': 0xa03b85920}
STATS, SLEN = 0xa03b88c18, 0x280   # engine 0 entry + head of engine 1, one read


def main():
    secs = float(sys.argv[1]) if len(sys.argv) > 1 else 40
    t, p = livectl.connect()
    rd = lambda a, n: p.debug.read(a, n)
    try:
        for k, a in G.items():
            print(f'{k:22s} {a:#x} = {struct.unpack("<I", rd(a, 4))[0]}')
        csv = HERE / 'logs' / time.strftime('c30probe-%H%M%S.csv'); csv.parent.mkdir(exist_ok=True)
        rows = []; t0 = time.monotonic(); prev = None
        print(f'sampling {secs:.0f} s ... (half-press any time)'); sys.stdout.flush()
        while time.monotonic() - t0 < secs:
            ts = time.monotonic() - t0
            b = rd(STATS, SLEN)
            rows.append((ts, b))
            if prev is not None and b != prev and int(ts) != int(rows[-2][0]):
                pass
            prev = b
        with open(csv, 'w') as f:
            for ts, b in rows: f.write(f'{ts:.3f},{b.hex()}\n')
        # per-second: samples, changed samples, which 4-byte words changed
        print(f'{len(rows)} samples ({len(rows)/secs:.1f}/s) -> {csv}')
        print(' sec  samples  changes  words(eng:off)')
        words = {}
        for s in range(int(secs) + 1):
            sel = [i for i, (ts, _) in enumerate(rows) if int(ts) == s and i > 0]
            ch = 0; ws = set()
            for i in sel:
                a, b = rows[i - 1][1], rows[i][1]
                if a != b:
                    ch += 1
                    for o in range(0, len(b), 4):
                        if a[o:o + 4] != b[o:o + 4]:
                            w = f'{o // 0x240}:{o % 0x240:#x}'; ws.add(w); words[w] = words.get(w, 0) + 1
            print(f'{s:4d} {len(sel):8d} {ch:8d}  {" ".join(sorted(ws))[:90]}')
        print('word change totals:', dict(sorted(words.items(), key=lambda x: -x[1])[:20]))
        print('G_DRAINED now', struct.unpack('<I', rd(G['G_DRAINED'], 4))[0])
    finally:
        t.__exit__(None, None, None)


if __name__ == '__main__':
    main()
