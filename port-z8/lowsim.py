import re,struct
from pathlib import Path
from addrmap import Mapper,BASE
from capstone import Cs,CS_ARCH_ARM64,CS_MODE_ARM
md=Cs(CS_ARCH_ARM64,CS_MODE_ARM)
m=Mapper(); R=Path('..'); S=set()
fs=[f for f in (R/'src/platform/z9_c532').iterdir() if f.name!='usb_debug.c']+[R/'host/build_z9_core.py']
for f in fs:
    for h in re.findall(r'0x([0-9a-fA-F]{8})(?![0-9a-fA-F])',f.read_text(encoding='utf-8')):
        v=int(h,16)
        if 0x40160000<=v<0x42000000: S.add(v)
print(len(S))
def dis(d,a):
    return '; '.join(f'{x.mnemonic} {x.op_str}' for x in md.disasm(d[a-BASE:a-BASE+16],a))
for v in sorted(S):
    if v in(0x401a0000,0x40200000): continue
    r=m.code(v)
    if not r or r['sim']<0.8:
        print(hex(v),r and hex(r['z8']),r and (r['sim'],r['votes'],r['second']));print('  z9',dis(m.d9,v))
        if r:print('  z8',dis(m.d8,r['z8']))
# Z9 page678 vs Z8 page704 event compare
off=lambda D,va:(va-struct.unpack_from('<Q',D,0x80d8)[0]+struct.unpack_from('<Q',D,0x8148)[0]) if va>=0xa00000000 else va-BASE
def ev(D,F,p):
    e=F+(p-1)*96; desc=struct.unpack_from('<Q',D,e+80)[0]; _,_,h,rw,evs=struct.unpack_from('<IIQQQ',D,off(D,desc)); q=struct.unpack_from('<Q',D,off(D,evs))[0]
    return [struct.unpack_from('<QQQQ',D,off(D,q)+32*i) for i in range(20)]
for a,b in zip(ev(m.d9,79632912,678),ev(m.d8,0x4bffd20,704)):
    print(hex(a[0]),hex(a[1]),'->',hex(m.code(a[1])['z8']) if a[1] else 0,hex(a[3]),'|',hex(b[0]),hex(b[1]),hex(b[3]))
