"""Call the stock C0 snapshot 0x40c269cc(0x15431000, buf) a few times (read-only register copy,
the same call Live makes per frame) and print the 33 words; scratch is a heap block freed at the end."""
import struct,sys,time
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
import livectl as L, z8_audio_probe as zp
t,p=L.connect()
try:
    buf=zp.native_call(p,0x40d2c408,(512,0))&0xffffffffffffffff
    rows=[]
    for i in range(int(sys.argv[1]) if len(sys.argv)>1 else 8):
        p.debug.write(buf,bytes(264))
        zp.native_call(p,0x40c269cc,(0x15431000,buf))
        w=struct.unpack('<33Q',p.debug.read(buf,264))
        seq=struct.unpack('<Q',p.debug.read(L.engine(p)+1688,8))[0]
        rows.append((seq,w))
    zp.native_call(p,0x40d2c448,(buf,))
    for seq,w in rows:
        print('seq',seq,' '.join(f'{i}:{v:#x}' for i,v in enumerate(w) if v>=0x800000000))
finally:
    t.__exit__(None,None,None)
