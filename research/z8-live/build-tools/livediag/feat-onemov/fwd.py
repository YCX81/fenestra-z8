import sys, struct, capstone
B=0x40160000
import os;d=open(os.environ.get('FW','/path/to/user/fxfix/v53-root/v53g/q/main.main'),'rb').read()
md=capstone.Cs(capstone.CS_ARCH_ARM64,capstone.CS_MODE_ARM)
def dis(a,n):
    for i in md.disasm(d[a-B:a-B+4*n],a):
        print(f"{i.address:x}: {i.mnemonic:7s} {i.op_str}")
def callers(t):
    out=[]
    for o in range(0,0x1e00000,4):
        w=struct.unpack_from('<I',d,o)[0]
        if w>>26 in (0x25,0x05):
            off=w&0x3ffffff
            if off&0x2000000: off-=0x4000000
            if B+o+off*4==t: out.append((B+o,'bl' if w>>26==0x25 else 'b'))
    return out
if __name__=='__main__':
    if sys.argv[1]=='c':
        for t in sys.argv[2:]:
            print(t,[f"{a:x}:{k}" for a,k in callers(int(t,16))])
    else:
        dis(int(sys.argv[1],16),int(sys.argv[2]))
