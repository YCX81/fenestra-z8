#!/usr/bin/env python3
"""Read-only: live-view frame rate over time (Live runtime capture.irq_seq and stream counters), 1 row per second.
  python lvrate.py [SECONDS]   default 30; half-press during the run"""
import sys, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent)); import livectl
secs = float(sys.argv[1]) if len(sys.argv) > 1 else 30
t, p = livectl.connect()
try:
    eng = livectl.engine(p); t0 = time.monotonic(); prev = None
    print(' sec  lv_irq/s  encoded/s  copied/s')
    while time.monotonic() - t0 < secs:
        s = livectl.state(p, eng); now = time.monotonic()
        cur = (now, s.get('capture.irq_seq', 0), s.get('stream.encoded', 0), s.get('capture.copied', 0))
        if prev:
            dt = cur[0] - prev[0]
            print(f'{now - t0:5.1f} {(cur[1]-prev[1])/dt:9.1f} {(cur[2]-prev[2])/dt:10.1f} {(cur[3]-prev[3])/dt:9.1f}', flush=True)
        prev = cur; time.sleep(max(0, 1 - (time.monotonic() - now)))
finally:
    t.__exit__(None, None, None)
