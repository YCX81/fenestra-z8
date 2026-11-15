"""Read-only disassembly of the exact v52 G firmware used on the camera."""
from pathlib import Path
import hashlib, struct, sys
import capstone
B=0x40160000
P=Path(__file__).with_name('v52g.main')
d=P.read_bytes()
assert hashlib.sha256(d).hexdigest()=='ed1e8496fccde443412e527a252de5029a55c749053b9b046da42bfc454aa613'
md=capstone.Cs(capstone.CS_ARCH_ARM64,capstone.CS_MODE_ARM)
def dis(a,n):
    for i in md.disasm(d[a-B:a-B+4*n],a):
        print(f'{i.address:08x}: {i.mnemonic:8s} {i.op_str}')
def target(a):
    w=struct.unpack_from('<I',d,a-B)[0]
    if w>>26 not in (0x25,0x05):return None
    off=w&0x3ffffff
    if off&0x2000000:off-=0x4000000
    return a+off*4
def callers(t):
    return references().get(t,[])
_references=None
def references():
    global _references
    if _references is None:
        _references={}
        for i,(w,) in enumerate(struct.iter_unpack('<I',d[:0x1e00000])):
            if w>>26 not in (0x25,0x05):continue
            off=w&0x3ffffff
            if off&0x2000000:off-=0x4000000
            a=B+4*i
            _references.setdefault(a+4*off,[]).append(a)
    return _references
if __name__=='__main__':
    if sys.argv[1]=='c':
        for t in sys.argv[2:]:print(t,[hex(c) for c in callers(int(t,16))])
    elif sys.argv[1]=='stubs':
        for a in range(0x40cf4034,0x40cf4300,4):
            t=target(a)
            if t: print(f'{a:x} -> {t:x}',[hex(c) for c in callers(a)])
    else:dis(int(sys.argv[1],16),int(sys.argv[2]))
