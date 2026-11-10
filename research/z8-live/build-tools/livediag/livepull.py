#!/usr/bin/env python3
"""One-step Live check over Fenestra USB Debug (v9 builds), no card removal.
Pulls the newest Live clip(s) from the card, saves the flight-recorder log next to them,
and runs the tear check (tear.py in WSL) on each MOV.

  python livepull.py                  newest Live clip (from the last written path)
  python livepull.py --latest 3       the three newest clips
  python livepull.py DSC_9720         a given clip
  python livepull.py --jpg ...        also pull the Live JPG (~16 MB, about 8 min at USB-debug speed)
Output: pulled\\<DSC>\\ (MOV, optional JPG, flight.csv, tear.txt)."""
import argparse
import re
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import livectl  # noqa: E402
import z8card  # noqa: E402

WSL_TEAR = '/path/to/user/fxfix/tear/tear.py'
WSL_PY = '/path/to/user/.local/share/nikon-z8-qemu/venv/bin/python'


def wsl_path(p):
    p = str(Path(p).resolve())
    return '/mnt/' + p[0].lower() + p[2:].replace('\\', '/')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('names', nargs='*', help='DSC_nnnn names; default: newest')
    ap.add_argument('--latest', type=int, default=1)
    ap.add_argument('--jpg', action='store_true')
    ap.add_argument('--folder', help='DCF folder, default: from the last written path')
    ap.add_argument('--out', default=str(HERE / 'pulled'))
    a = ap.parse_args()
    t, p = livectl.connect()
    c = None
    try:
        eng = livectl.engine(p)
        st = livectl.state(p, eng)
        last = st.get('last_native_path', '')
        names = list(a.names)
        if not a.folder:
            m = re.search(r'\\(\d{3}[A-Z0-9_]{5})\\', last)
            a.folder = m.group(1) if m else '100NCZ_8'
        c = z8card.Card(p, HERE / 'logs')
        if not names:
            m = re.search(r'DSC_(\d{4})', last)
            if not m:
                raise SystemExit(f'no last written path in the Live runtime ({last!r}); name the clip, e.g. DSC_9720')
            n = int(m.group(1))
            while len(names) < a.latest and n > 0:
                if c.exists(f'A:\\LIVE\\{a.folder}\\DSC_{n:04d}.MOV'):
                    names.append(f'DSC_{n:04d}')
                n -= 1
        out_root = Path(a.out)
        for name in names:
            d = out_root / name
            d.mkdir(parents=True, exist_ok=True)
            mov = d / f'{name}.MOV'
            t0 = time.monotonic()
            size = c.get(f'A:\\LIVE\\{a.folder}\\{name}.MOV', str(mov))
            print(f'{name}.MOV {size} B in {time.monotonic() - t0:.0f} s -> {mov}')
            if a.jpg:
                size = c.get(f'A:\\LIVE\\{a.folder}\\{name}.JPG', str(d / f'{name}.JPG'))
                print(f'{name}.JPG {size} B')
        if names and livectl.RT_SIZE in (18824, 18848, 18864, 20472, 20616, 21544, 21616, 23184):
            csv = out_root / names[0] / 'flight.csv'
            livectl.flight(p, eng, 20, str(csv))
    finally:
        if c is not None:
            c.release()
        t.__exit__(None, None, None)
    for name in names:
        mov = Path(a.out) / name / f'{name}.MOV'
        r = subprocess.run(['wsl', '-d', 'Ubuntu-26.04', '--', WSL_PY, WSL_TEAR, wsl_path(mov)],
                           capture_output=True, text=True)
        txt = r.stdout + r.stderr
        (mov.parent / 'tear.txt').write_text(txt)
        print(f'--- tear check {name}:\n{txt.strip()[-2000:]}')


if __name__ == '__main__':
    main()
