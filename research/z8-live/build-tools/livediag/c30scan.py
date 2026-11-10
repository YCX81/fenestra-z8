#!/usr/bin/env python3
"""Read-only scan of the HSFC (C15/C30) globals for words that change during a half-press.
Reads REGIONS repeatedly for SECONDS (debug memory reads only), then prints a per-second timeline of how
many words changed and, for the busiest words, their value per second (a ring head or frame counter shows
up as +30/s in C30 while the half-press is held).
  python c30scan.py [SECONDS]        default 60; raw -> logs/c30scan-<time>.npz-like csv"""
import struct, sys, time
from pathlib import Path
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import livectl

REGIONS = [(0xa03bc5000, 0x2000)]      # G_DRAINED 0xa03bc5638 .. G_MODE 0xa03bc6bc8 / G_LIMIT / budget
CH = 1000


def snap(rd):
    out = b''
    for a, n in REGIONS:
        for o in range(0, n, CH): out += rd(a + o, min(CH, n - o))
    return out


def addr(i):
    for a, n in REGIONS:
        if i < n: return a + i
        i -= n


def main():
    secs = float(sys.argv[1]) if len(sys.argv) > 1 else 60
    t, p = livectl.connect(); rd = lambda a, n: p.debug.read(a, n)
    try:
        print('G_MODE', struct.unpack('<I', rd(0xa03bc6bc8, 4))[0], 'G_LIMIT', struct.unpack('<I', rd(0xa03bc6bac, 4))[0])
        rows = []; t0 = time.monotonic()
        print(f'scanning {secs:.0f} s ...'); sys.stdout.flush()
        while time.monotonic() - t0 < secs: rows.append((time.monotonic() - t0, snap(rd)))
        print('G_DRAINED after', struct.unpack('<I', rd(0xa03bc5638, 4))[0])
    finally:
        t.__exit__(None, None, None)
    csv = HERE / 'logs' / time.strftime('c30scan-%H%M%S.csv'); csv.parent.mkdir(exist_ok=True)
    with open(csv, 'w') as f:
        for ts, b in rows: f.write(f'{ts:.3f},{b.hex()}\n')
    n = len(rows[0][1]) // 4
    val = lambda r, k: struct.unpack_from('<I', rows[r][1], 4 * k)[0]
    changes = [0] * n; persec = {}
    for r in range(1, len(rows)):
        s = int(rows[r][0])
        for k in range(n):
            if val(r, k) != val(r - 1, k):
                changes[k] += 1; persec.setdefault(s, set()).add(k)
    print(f'{len(rows)} samples ({len(rows)/secs:.1f}/s) -> {csv}')
    print(' sec  changed-words')
    for s in range(int(secs) + 1): print(f'{s:4d}  {len(persec.get(s, ()))}')
    top = sorted((k for k in range(n) if changes[k]), key=lambda k: -changes[k])[:24]
    print('busiest words, value at the first sample of each second:')
    firsts = {}
    for r, (ts, _) in enumerate(rows): firsts.setdefault(int(ts), r)
    for k in top:
        series = ' '.join(str(val(firsts[s], k)) for s in sorted(firsts))
        print(f'{addr(4 * k):#x} changes={changes[k]:4d}: {series[:200]}')


if __name__ == '__main__':
    main()
