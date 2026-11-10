"""Remove the v13 'orientation=' line from Live manifests (the playback parser rejects unknown keys).
Backs each original up locally, writes the fixed file, reads it back and compares.  python manfix.py STEM..."""
import sys, io, contextlib
from pathlib import Path
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE)); import livectl, z8card
BK = Path(r'C:\Users\ycx\Nikon-probe\man-backup'); BK.mkdir(exist_ok=True)
t, p = livectl.connect(); c = z8card.Card(p, HERE / 'logs')
try:
    for stem in sys.argv[1:]:
        path = f'A:\\LIVEDATA\\101NCZ_8\\{stem}.MAN'; orig = BK / f'{stem}.MAN.orig'; new = BK / f'{stem}.MAN.fixed'; chk = BK / f'{stem}.MAN.check'
        for q in (orig, chk):
            if q.exists(): q.unlink()
        with contextlib.redirect_stdout(io.StringIO()): c.get(path, str(orig))
        b = orig.read_bytes(); lines = b.split(b'\n')
        keep = [l for l in lines if not l.startswith(b'orientation=')]
        if len(keep) == len(lines): print(stem, 'already 4 keys,', len(b), 'B'); continue
        fixed = b'\n'.join(keep); assert fixed.count(b'\n') == 4 and fixed.startswith(b'native='), fixed
        new.write_bytes(fixed)
        with contextlib.redirect_stdout(io.StringIO()): c.put(str(new), path, True); c.get(path, str(chk))
        print(stem, len(b), '->', len(fixed), 'B', 'verified' if chk.read_bytes() == fixed else 'MISMATCH')
finally:
    c.release(); t.__exit__(None, None, None)
