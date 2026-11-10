"""Read-only: list the newest Live sets (LIVE/<folder>/DSC_nnnn.MOV) by probing numbers upward from START."""
import sys, io, contextlib
from pathlib import Path
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE)); import livectl, z8card
folder, start = sys.argv[1], int(sys.argv[2])
t, p = livectl.connect(); c = z8card.Card(p, HERE / 'logs')
try:
    miss = 0; n = start; found = []
    while miss < 6:
        if c.exists(f'A:\\LIVE\\{folder}\\DSC_{n:04d}.MOV'): found.append(n); miss = 0
        elif c.exists(f'A:\\DCIM\\{folder}\\DSC_{n:04d}.JPG') or c.exists(f'A:\\DCIM\\{folder}\\DSC_{n:04d}.NEF'): miss = 0
        else: miss += 1
        n += 1
    print('Live sets:', found)
finally:
    c.release(); t.__exit__(None, None, None)
