import struct
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from addrmap import Z9, Z8, BASE
md=Cs(CS_ARCH_ARM64,CS_MODE_ARM)
for name,p,a in (('z9',Z9,0x4032e57c),('z8',Z8,0x4032e130)):
    d=p.read_bytes(); print(name)
    for i in md.disasm(d[a-BASE:a-BASE+0xa0],a): print(f' {i.address:x} {i.mnemonic} {i.op_str}')
