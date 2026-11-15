import struct,collections
from pathlib import Path
LOAD=0x40160000
d=(Path.home()/"fs-out/stock.main").read_bytes()
hits=collections.defaultdict(list)
for i in range(0,len(d)-4,4):
    v=struct.unpack_from("<I",d,i)[0]
    if v>>16==0x4204: hits[v].append(LOAD+i)
for k in sorted(hits): print(hex(k),len(hits[k])," ".join(hex(a) for a in hits[k][:6]))
