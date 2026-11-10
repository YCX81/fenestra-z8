"""Heap headroom test: native heap 0x40d2c408(n,0) then free 0x40d2c448 at once."""
import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
import livectl as L, z8_audio_probe as zp
t,p=L.connect()
try:
    for kb in [int(x) for x in sys.argv[1:]]:
        a=zp.native_call(p,0x40d2c408,(kb<<10,0))&0xffffffffffffffff
        if not a: print(f'{kb} KB: failed');break
        r=p.debug.translate(a);zp.native_call(p,0x40d2c448,(a,))
        print(f'{kb} KB: ok at {a:#x} attr={r["attribute"]:#04x} (freed)')
finally:
    t.__exit__(None,None,None)
