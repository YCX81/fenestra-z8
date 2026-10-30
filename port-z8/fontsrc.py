import numpy as np, struct
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from addrmap import BASE, Mapper
m=Mapper(); md=Cs(CS_ARCH_ARM64,CS_MODE_ARM)
def callers(w,t):
    bl=np.nonzero(((w>>26)==0x25)|((w>>26)==0x05))[0]; im=(w[bl]&0x3ffffff).astype(np.int64); im[im>=1<<25]-=1<<26
    return [int(i) for i,dd in zip(bl,im) if BASE+4*int(i)+4*int(dd)==t]
for t in (0x4032e4c0,0x4032e4a8):
    for i in callers(m.w9,t):
        a=BASE+4*i; print('z9 caller',hex(a))
        for x in md.disasm(m.d9[a-BASE-0x30:a-BASE+8],a-0x30): print('  ',hex(x.address),x.mnemonic,x.op_str)
