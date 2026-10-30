"""Translate model constants: find Z9 mov-immediate sites, map them, read the Z8 immediate."""
import struct, numpy as np
from addrmap import Z9, Z8, BASE, Mapper
m=Mapper()
def off(d,va):
    f=struct.unpack_from('<Q',d,0x8148)[0]; r=struct.unpack_from('<Q',d,0x80d8)[0]
    return va-r+f if va>=0xa00000000 else va-BASE
def immsites(w,v):
    a=(w&0x7fe00000)==0x52800000
    return np.nonzero(a&(((w>>5)&0xffff)==v))[0]
for v in (0xc84,0x4bec,0x4bed,0x4bf1,705,656,792,0x6b8,0x6b9,0x4db,0x27e):
    from collections import Counter
    c=Counter(); s=immsites(m.w9,v)
    for i in s[:60]:
        r=m.code(BASE+4*int(i))
        if not r or r['sim']<0.8: c['?']+=1; continue
        j=(r['z8']-BASE)//4; x=int(m.w8[j])
        c[(x>>5)&0xffff if (x&0x7fe00000)==0x52800000 else 'diff']+=1
    print(hex(v),v,'sites',len(s),c.most_common(5))
# Z8 nav table: search 0x290 record with mapped callback
cb=m.code(0x403b20c4)['z8']; pat=struct.pack('<QQ',0x290,cb)
print('z8 cb',hex(cb)); o=m.d8.find(pat); print('nav file',hex(o),'va',hex(o+BASE) if o>=0 else '')
while o>=0:
    print('nav @',hex(o+BASE))
    for i in range(18):
        e=struct.unpack_from('<QQQQ',m.d8,o+32*i); print('  ',[hex(x) for x in e])
        if e[1]==0 and e[2]==0 and i>2 and e[0]!=0x318: break
    o=m.d8.find(pat,o+1); break
# wanted pages
pf=79632912
print('template page idx',(0xa0290c7f0-(0xa02808000+pf-struct.unpack_from('<Q',m.d9,0x8148)[0]))/96+1)
