"""Read-only: dump bytes of the Live runtime at an offset.  python rtpeek.py OFFSET LEN"""
import sys, struct
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent)); import livectl
t, p = livectl.connect()
try:
    eng = livectl.engine(p); off, n = int(sys.argv[1], 0), int(sys.argv[2], 0)
    b = p.debug.read(eng + off, n); print(f'eng {eng:#x} +{off}:', b.hex())
    print('as u32:', struct.unpack(f'<{n // 4}I', b[:n // 4 * 4]))
finally:
    t.__exit__(None, None, None)
