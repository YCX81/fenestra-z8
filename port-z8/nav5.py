import struct, numpy as np
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from addrmap import BASE, Mapper
m=Mapper(); md=Cs(CS_ARCH_ARM64,CS_MODE_ARM)
def show(d,a,n):
    for x in md.disasm(d[a-BASE:a-BASE+n],a): print('  ',hex(x.address),x.mnemonic,x.op_str)
# Fenestra constant TEXT: who writes the font root? Z9 refs via movz chain
sites=m.__class__._movchains(m,m.w9); s=sites.get(0xa02f4ab30,[]); print('font root sites',[hex(BASE+4*i) for i in s])
for i in s:
    a=BASE+4*i; r=m.code(a); print(hex(a),'->',hex(r['z8']),r['sim'])
# search immediates 3204*k in Z9 near font init
for v in (3204,3203,3205,3204*8&0xffff,3204*4):
    for name,w in (('z9',m.w9),):
        hit=np.nonzero(((w&0x7fe00000)==0x52800000)&(((w>>5)&0xffff)==v))[0]
        hit2=np.nonzero(((w&0x7f800000)==0x11000000)&(((w>>10)&0xfff)==v&0xfff)&(v<4096))[0]
        cmp2=np.nonzero(((w&0x7f80001f)==0x7100001f)&(((w>>10)&0xfff)==(v&0xfff))&(v<4096))[0]
        print(v,'mov',[hex(BASE+4*int(i)) for i in hit][:10],'cmp',[hex(BASE+4*int(i)) for i in cmp2][:10])
