"""Headroom: pool blocks (Live class) of given MB, and the native heap holding 1 MB + 3 MB at once. All freed."""
import struct,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
import livectl as L, z8_audio_probe as zp
t,p=L.connect()
try:
    for mb in (40,50,64):
        if struct.unpack('<I',p.debug.read(0xa03f31a68,4))[0]:print('gate set');break
        d=zp.native_call(p,0x4104d2dc,(0x4c41,0x60,mb<<20))&0xffffffffffffffff
        print(f'pool {mb} MB:', 'ok' if d else 'failed')
        if d: zp.native_call(p,0x4104d600,(d,))
        else: break
    a=zp.native_call(p,0x40d2c408,(1<<20,0))&0xffffffffffffffff
    b=zp.native_call(p,0x40d2c408,(3<<20,0))&0xffffffffffffffff if a else 0
    print('heap 1 MB + 3 MB:', 'ok' if a and b else 'failed', hex(a), hex(b))
    if b: zp.native_call(p,0x40d2c448,(b,))
    if a: zp.native_call(p,0x40d2c448,(a,))
finally:
    t.__exit__(None,None,None)
