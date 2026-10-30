import struct
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from addrmap import BASE, Mapper
m=Mapper(); md=Cs(CS_ARCH_ARM64,CS_MODE_ARM)
def args(d,a,n):
    vals=[];cur=0;tgt=None
    for x in md.disasm(d[a-BASE:a-BASE+n],a):
        if x.mnemonic=='mov' and x.op_str.startswith('x0'): cur=int(x.op_str.split('#')[1],16)
        elif x.mnemonic=='movk' and x.op_str.startswith('x0'):
            p=x.op_str.split(', '); v=int(p[1][1:],16); s=int(p[2].split('#')[1]) if len(p)>2 else 0; cur|=v<<s
        elif x.mnemonic=='bl': vals.append((hex(x.address),x.op_str,hex(cur))); cur=0
    return vals
r=m.code(0x403e9674); print(r)
A=args(m.d9,0x403e95c0,0x160); B=args(m.d8,r['z8']-0xb4,0x160)
print('z9');[print(' ',v) for v in A]; print('z8');[print(' ',v) for v in B]
def sz(d,fa):
    # font table: u8 per text. Find first index where a run of values looks out of range
    v=d[fa-BASE:fa-BASE+5000]; return v[:8].hex(), v[3195:3215].hex()
