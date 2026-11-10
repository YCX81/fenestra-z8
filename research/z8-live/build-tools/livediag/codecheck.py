import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent)); import livectl
addr, v13, v131 = open(r'C:\Users\ycx\Nikon-probe\vercheck.txt').read().split()
t, p = livectl.connect()
try:
    got = p.debug.read(int(addr, 16), 16).hex()
    print('running', got, '=> v34' if got == v13 else '=> v35' if got == v131 else '=> unknown')
finally:
    t.__exit__(None, None, None)
