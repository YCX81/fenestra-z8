import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent)); import livectl
t, p = livectl.connect()
try:
    print('running identity', p.debug.read(0x40160000 + 0x14eaa90, 8))
    # NFP v0.4 writer handler code differs from v0.3 at 0x409f262c region
    print('0x409f262c', p.debug.read(0x409f262c, 8).hex())
    eng = livectl.engine(p); print('Live runtime size', livectl.RT_SIZE)
finally:
    t.__exit__(None, None, None)
