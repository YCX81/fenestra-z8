import struct,sys
from pathlib import Path
LOAD=0x40160000
d=(Path.home()/"fs-out/stock.main").read_bytes()
a=int(sys.argv[1],16);n=int(sys.argv[2])
for i in range(n):
    off=a-LOAD+i*8
    lo,hi=struct.unpack_from("<II",d,off)
    print(hex(a+i*8),"%08x %08x"%(lo,hi))
