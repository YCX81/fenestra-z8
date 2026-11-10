"""Pool headroom test: allocate one block from Live's native pool class (0x4104d2dc(0x4c41,0x60,n)),
read its size, free it at once (0x4104d600). Refuses while the still-capture gate is set."""
import struct,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
import livectl as L, z8_audio_probe as zp
t,p=L.connect()
try:
    for mb in [int(x) for x in sys.argv[1:]]:
        gate=struct.unpack('<I',p.debug.read(0xa03f31a68,4))[0]
        if gate: print('pool gate set, skipped');break
        d=zp.native_call(p,0x4104d2dc,(0x4c41,0x60,mb<<20))&0xffffffffffffffff
        if not d: print(f'{mb} MB: allocation failed');break
        r=struct.unpack('<Q',p.debug.read(d+8,8))[0];cls=struct.unpack('<I',p.debug.read(d+0x10,4))[0]
        base,size=struct.unpack('<QQ',p.debug.read(r,16)) if r else (0,0)
        zp.native_call(p,0x4104d600,(d,))
        print(f'{mb} MB: ok desc={d:#x} class={cls:#x} base={base:#x} size={size>>20} MB (freed)')
finally:
    t.__exit__(None,None,None)
