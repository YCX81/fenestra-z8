import struct, numpy as np
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from addrmap import Z9, Z8, BASE, Mapper
m=Mapper(); md=Cs(CS_ARCH_ARM64,CS_MODE_ARM)
def recs(d,off,n=18):
    return [struct.unpack_from('<QQQQ',d,off+32*i) for i in range(n)]
def find(d):
    a=np.frombuffer(d[:len(d)//8*8],dtype='<u8'); out=[]
    idx=np.nonzero(a==0x290)[0]
    for i in idx:
        i=int(i)
        if i+12<len(a) and a[i+4]==0x291 and a[i+8]==0x295 and a[i+12]==0x296: out.append(i*8)
    return out
for name,d in (('z9',m.d9),('z8',m.d8)):
    h=find(d); print(name,len(h),'tables')
    from collections import Counter
    c=Counter(d[o:o+32*15] for o in h); print(' distinct',len(c))
    if name=='z9': print(' nav present',(0x43014bb8-BASE) in h)
    if name=='z8':
        mapped=[(m.code(x)['z8']) for x in (0x403b20c4,0x403b20c8,0x403b208c)]
        print(' mapped cb',[hex(x) for x in mapped],'cond',hex(m.code(0x403b217c)['z8']),hex(m.code(0x403b2194)['z8']))
        sel=[o for o in h if struct.unpack_from('<Q',d,o+8)[0]==mapped[0]]
        print(' cb-match',[hex(o+BASE) for o in sel][:10])
        for o in sel[:40]:
            r=recs(d,o,16)
            s=[(hex(e[0]),hex(e[3])) for e in r]
            print(hex(o+BASE),s[:15]);break
        tgt=recs(m.d9,0x43014bb8-BASE,15)
        Tk=[(e[0],e[1] and m.code(e[1])['z8'],e[2] and m.code(e[2])['z8']) for e in tgt]
        print(' want',[(hex(a),hex(b),hex(c)) for a,b,c in Tk])
        best=[]
        for o in h:
            r=recs(d,o,15); k=sum((e[0],e[1],e[2])==t for e,t in zip(r,Tk)); best.append((k,o))
        best.sort(reverse=True); print(' best',[(k,hex(o+BASE)) for k,o in best[:6]])
        k,o=best[0]
        for e in recs(d,o,16): print('   ',[hex(x) for x in e])
        # stock Setup row 42 events first
# text count
for a in (0x402d7204,0x40f3f66c):
    i=(a-BASE)//4; print(hex(a)); 
    for x in md.disasm(m.d9[a-BASE-24:a-BASE+28],a-24): print('  ',hex(x.address),x.mnemonic,x.op_str)
    r=m.code(a); print('  ->',r)
