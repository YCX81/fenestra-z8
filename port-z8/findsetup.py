import struct
from addrmap import Z8, Z9, BASE
def scan(p, first, stop):
    d=p.read_bytes(); f=struct.unpack_from('<Q',d,0x8148)[0]; r=struct.unpack_from('<Q',d,0x80d8)[0]
    off=lambda va: va-r+f if va>=0xa00000000 else va-BASE
    out=[]
    for page in range(1,stop):
        e=first+(page-1)*96; ident=struct.unpack_from('<I',d,e)[0]
        if ident&0xffff!=2: continue
        desc=struct.unpack_from('<Q',d,e+80)[0]
        if not 0xa02808000<=desc<0xa02808000+0x300000: continue
        _,_,hdr,rows,evs=struct.unpack_from('<IIQQQ',d,off(desc)); cnt=struct.unpack_from('<H',d,off(desc)+32)[0]
        if not (0x40160000<=rows<0x46000000) or cnt>200: continue
        w=[struct.unpack_from('<I',d,off(rows)+44*i)[0] for i in range(cnt)]
        if 651 in w or 906 in w: out.append((page,hex(desc),cnt,hex(hdr),hex(rows),hex(evs),w[:3],w.index(906) if 906 in w else None))
    return out
for x in scan(Z8,0x4bffd20,0x6a0): print('z8',x)
for x in scan(Z9,79632912,0x6b8): print('z9',x)
