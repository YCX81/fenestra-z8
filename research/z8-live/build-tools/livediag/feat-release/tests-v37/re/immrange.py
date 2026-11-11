import struct,sys
from pathlib import Path
LOAD=0x40160000
d=(Path.home()/"fs-out/stock.main").read_bytes()
w=struct.unpack_from("<%dI"%(0x3000000//4),d,0)
lo_,hi_=int(sys.argv[1],16),int(sys.argv[2],16)
for i,x in enumerate(w[:-1]):
    if x&0x7fe00000==0x52800000:
        y=w[i+1]
        if y&0x7fe00000==0x72a00000 and (x&31)==(y&31):
            v=((y>>5)&0xffff)<<16|((x>>5)&0xffff)
            if lo_<=v<hi_: print(hex(LOAD+i*4),hex(v))
