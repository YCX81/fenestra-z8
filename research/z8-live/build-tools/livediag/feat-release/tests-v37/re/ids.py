import struct,sys,collections
from pathlib import Path
LOAD=0x40160000
d=(Path.home()/"fs-out/stock.main").read_bytes()
w=struct.unpack_from("<%dI"%(0x3000000//4),d,0)
hi=int(sys.argv[1],16)
found=collections.defaultdict(list)
for i,x in enumerate(w[:-1]):
    y=w[i+1]
    if x&0x7fe00000==0x52800000 and y&0x7fe00000==0x72a00000 and (y>>5)&0xffff==hi and (x&31)==(y&31):
        found[(hi<<16)|((x>>5)&0xffff)].append(LOAD+i*4)
    if x&0x7fe00000==0x52a00000 and (x>>5)&0xffff==hi:
        if y&0x7fe00000==0x72800000 and (x&31)==(y&31): found[(hi<<16)|((y>>5)&0xffff)].append(LOAD+i*4)
        else: found[hi<<16].append(LOAD+i*4)
for k in sorted(found): print(hex(k),len(found[k])," ".join(hex(a) for a in found[k][:10]))
