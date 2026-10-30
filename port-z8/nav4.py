import struct, numpy as np
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from addrmap import BASE, Mapper
m=Mapper(); md=Cs(CS_ARCH_ARM64,CS_MODE_ARM)
D=lambda d:(struct.unpack_from('<Q',d,0x8148)[0],struct.unpack_from('<Q',d,0x80d8)[0])
def off(d,va):
    f,r=D(d); return va-r+f if va>=0xa00000000 else va-BASE
F9,F8=79632912,0x4bffd20
# nav table: Z9 cbs mapped, events -0x12
T=[struct.unpack_from('<QQQQ',m.d9,0x43014bb8-BASE+32*i) for i in range(15)]
c=lambda x: m.code(x)['z8'] if x else 0
want=[(t[0]-0x12,c(t[1]),c(t[2])) for t in T[:14]]
a=np.frombuffer(m.d8[:len(m.d8)//8*8],dtype='<u8')
hits=[]
for i in np.nonzero(a==want[0][0])[0]:
    i=int(i); 
    if i+56>=len(a): continue
    if all((int(a[i+4*k]),int(a[i+4*k+1]),int(a[i+4*k+2]))==want[k] for k in range(14)): hits.append(i*8)
print('nav hits',[hex(h+BASE) for h in hits])
for h in hits[:1]:
    for k in range(15): print('  ',[hex(x) for x in struct.unpack_from('<QQQQ',m.d8,h+32*k)])
print('z9 nav pointer holders', hex(0x2eae6b0+BASE))
# page lookup
def page_of_desc(d,first,desc,stop):
    return [p for p in range(1,stop) if struct.unpack_from('<Q',d,first+(p-1)*96+80)[0]==desc]
def info(d,first,p):
    e=first+(p-1)*96; desc=struct.unpack_from('<Q',d,e+80)[0]
    fl,_,h,r,ev=struct.unpack_from('<IIQQQ',d,off(d,desc)); n=struct.unpack_from('<H',d,off(d,desc)+32)[0]
    return dict(page=p,kind=struct.unpack_from('<I',d,e)[0]&0xffff,desc=hex(desc),flags=hex(fl),header=hex(h),rows=hex(r),events=hex(ev),n=n,cb=[hex(x) for x in struct.unpack_from('<Q',d,e+40)])
def rowcbs(d,first,p):
    e=first+(p-1)*96; desc=struct.unpack_from('<Q',d,e+80)[0]
    fl,_,h,r,ev=struct.unpack_from('<IIQQQ',d,off(d,desc)); n=struct.unpack_from('<H',d,off(d,desc)+32)[0]
    out=[]
    for i in range(min(n,8)):
        q=struct.unpack_from('<Q',d,off(d,ev)+8*i)[0]
        for k in range(8):
            ee=struct.unpack_from('<QQQQ',d,off(d,q)+32*k); out.append(ee[1])
    return fl&0xff,n,out
def match(pz9):
    fl,n,cb=rowcbs(m.d9,F9,pz9); mc=[(m.code(x) or {}).get('z8',0) if x else 0 for x in cb]
    best=[]
    for p in range(1,0x68c):
        if struct.unpack_from('<I',m.d8,F8+(p-1)*96)[0]==0: continue
        try:
            f2,n2,c2=rowcbs(m.d8,F8,p)
        except Exception: continue
        if f2!=fl or n2!=n: continue
        best.append((sum(a==b for a,b in zip(mc,c2)),p))
    best.sort(reverse=True); return (fl,n,best[:4])
dlg=page_of_desc(m.d9,F9,0xa028be460,0x6b8); print('dialog desc pages',dlg)
for p in dlg[:3]+[678,1243]:
    print('z9',info(m.d9,F9,p)); r=match(p); print('  match',r)
    if r[2]:print('  z8',info(m.d8,F8,r[2][0][1]))
print('rows 0x42e9b81c file', hex(0x42e9b81c-BASE), struct.unpack_from('<11I',m.d9,0x42e9b81c-BASE))
for a in (0x402d7204,0x40f3f66c):
    print(hex(a), m.code(a))
    for x in md.disasm(m.d9[a-BASE-20:a-BASE+24],a-20): print('  ',hex(x.address),x.mnemonic,x.op_str)
