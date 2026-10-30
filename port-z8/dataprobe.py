"""Initialized-data layout of both MAINs and the Setup page (638) descriptor chain."""
import struct, sys
from addrmap import Z9, Z8, BASE
def img(p):
    d=p.read_bytes(); f=struct.unpack_from('<Q',d,0x8148)[0]; r=struct.unpack_from('<Q',d,0x80d8)[0]; n=struct.unpack_from('<Q',d,0x80d0)[0]
    return d,f,r,n
def rd(d,f,r,va,fmt):
    off = va-r+f if va >= 0xa00000000 else va-BASE
    return struct.unpack_from(fmt,d,off)
for name,p,first in (('z9',Z9,79632912),('z8',Z8,0x4bffd20)):
    d,f,r,n=img(p); print(name,'data file',hex(f),'ram',hex(r),'bytes',hex(n))
    page=638; e=first+(page-1)*96
    ident=struct.unpack_from('<I',d,e)[0]; desc=struct.unpack_from('<Q',d,e+80)[0]
    print(' page',page,'ident',hex(ident),'desc',hex(desc))
    flags,_,hdr,rows,evs=rd(d,f,r,desc,'<IIQQQ'); cnt=rd(d,f,r,desc+32,'<H')[0]
    print(' desc header',hex(hdr),'rows',hex(rows),'events',hex(evs),'count',cnt)
    rw=[rd(d,f,r,rows+44*i,'<11I') for i in range(cnt)]
    print(' rows word0',[x[0] for x in rw]); print(' row42',rw[42])
    print(' hdr bytes',d[hdr-BASE:hdr-BASE+32].hex())
