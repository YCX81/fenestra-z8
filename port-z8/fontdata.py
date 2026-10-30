import struct, numpy as np
from capstone import Cs,CS_ARCH_ARM64,CS_MODE_ARM
from addrmap import BASE, Mapper
m=Mapper(); md=Cs(CS_ARCH_ARM64,CS_MODE_ARM)
def chains(w,target=None):
    out={}
    for i in np.nonzero((w&0xff800000)==0xd2800000)[0]:
        i=int(i); a=int(w[i]); rd=a&31; val=((a>>5)&0xffff)<<(16*((a>>21)&3))
        for k in range(1,6):
            b=int(w[i+k])
            if (b&0xff800000)==0xf2800000 and (b&31)==rd:
                sh=16*((b>>21)&3); val=(val&~(0xffff<<sh))|(((b>>5)&0xffff)<<sh)
        out.setdefault(val,[]).append(i)
    return out
c9=chains(m.w9); s=c9.get(0x42271338,[]); print('z9 sites',[hex(BASE+4*i) for i in s])
c8=None
for i in s:
    a=BASE+4*i; r=m.code(a); print(hex(a),r and hex(r['z8']),r and r['sim'])
    for x in md.disasm(m.d9[a-BASE-8:a-BASE+36],a-8): print('  z9',hex(x.address),x.mnemonic,x.op_str)
    if r:
        for x in md.disasm(m.d8[r['z8']-BASE-8:r['z8']-BASE+36],r['z8']-8): print('  z8',hex(x.address),x.mnemonic,x.op_str)
for a in (0x40884508,0x40883a38,0x40c7cb34): print(hex(a),m.code(a))
# size immediates
for v in (170765&0xffff,170765>>16):
    pass
print('blob head z9',m.d9[0x42271338-BASE:0x42271338-BASE+32].hex())
pat=m.d9[0x42271338-BASE:0x42271338-BASE+32]; o=m.d8.find(pat); print('same head in z8',hex(o+BASE) if o>=0 else None)
tail=m.d9[0x42271338-BASE+170765-16:0x42271338-BASE+170765]; print('tail',tail.hex())
if o>=0:
    eq=m.d8[o:o+170765]==m.d9[0x42271338-BASE:0x42271338-BASE+170765]; print('identical blob',eq)
