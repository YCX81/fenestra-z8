import struct, numpy as np
from addrmap import BASE, Mapper
m=Mapper()
d=m.d8; F8=0x4bffd20; F9=79632912
f,r=struct.unpack_from('<Q',d,0x8148)[0],struct.unpack_from('<Q',d,0x80d8)[0]
off=lambda D,va:(va-struct.unpack_from('<Q',D,0x80d8)[0]+struct.unpack_from('<Q',D,0x8148)[0]) if va>=0xa00000000 else va-BASE
A=struct.unpack_from('<60Q',d,0x439f6978-BASE); n=1
while n<60 and A[n]-A[n-1]==3136: n+=1
print('z8 languages',n)
# Z9 page 678 structure + its events
def pg(D,F,p):
    e=F+(p-1)*96; desc=struct.unpack_from('<Q',D,e+80)[0]
    fl,_,h,rw,ev=struct.unpack_from('<IIQQQ',D,off(D,desc)); c=struct.unpack_from('<H',D,off(D,desc)+32)[0]
    return desc,fl,h,rw,ev,c
desc,fl,h,rw,ev,c=pg(m.d9,F9,678); print('z9 678',hex(desc),hex(fl),hex(h),hex(rw),hex(ev),c)
q=struct.unpack_from('<Q',m.d9,off(m.d9,ev))[0]
E=[struct.unpack_from('<QQQQ',m.d9,off(m.d9,q)+32*i) for i in range(16)]
for e in E: print('  ',[hex(x) for x in e], hex(m.code(e[1])['z8']) if e[1] else '')
cbs={m.code(e[1])['z8'] for e in E[:13] if e[1]}
res=[]
for p in range(1,0x68c):
    if not struct.unpack_from('<I',d,F8+(p-1)*96)[0]&0xffff==2: continue
    try:
        de,fl2,h2,r2,e2,c2=pg(d,F8,p); q2=struct.unpack_from('<Q',d,off(d,e2))[0]
        E2=[struct.unpack_from('<QQQQ',d,off(d,q2)+32*i) for i in range(14)]
    except Exception: continue
    k=sum(x[1] in cbs for x in E2)
    if c2==1: res.append((k,p,hex(de),hex(fl2)))
res.sort(reverse=True); print('z8 best',res[:8])
# Z8 nav candidates with terminal
T=[struct.unpack_from('<QQQQ',m.d9,0x43014bb8-BASE+32*i) for i in range(15)]
c1=lambda x: m.code(x)['z8'] if x else 0
want=[(t[0]-0x12,c1(t[1]),c1(t[2])) for t in T[:14]]
a=np.frombuffer(d[:len(d)//8*8],dtype='<u8'); hits=[]
for i in np.nonzero(a==want[0][0])[0]:
    i=int(i)
    if i+60<len(a) and all((int(a[i+4*k]),int(a[i+4*k+1]),int(a[i+4*k+2]))==want[k] for k in range(14)): hits.append(i)
print('hits',len(hits)); from collections import Counter
print(Counter((hex(int(a[i+56])),hex(int(a[i+57])),hex(int(a[i+59]))) for i in hits).most_common(8))
term=[i for i in hits if a[i+56]==0x3b8]; print('terminal 0x3b8',[hex(i*8+BASE) for i in term][:5])
for i in term[:1]:
    for k in range(15): print('  ',[hex(int(x)) for x in a[i+4*k:i+4*k+4]])
print('z9 hits for comparison: nav next select',hex(T[5][3]))
# Z9 selected table pointer position: page row-events
def holder(D,va):
    b=np.frombuffer(D[:len(D)//8*8],dtype='<u8'); return [hex(int(x)*8+BASE) for x in np.nonzero(b==va)[0]][:5]
print(holder(m.d9,0x43014bb8))
if term: print(holder(d,term[0]*8+BASE))
print('--- 0x3bb tables')
T3=[i for i in hits if a[i+56]==0x3bb]
for i in T3: print(hex(i*8+BASE),'selnext',hex(int(a[i+23])),'t15 cb',hex(int(a[i+57])))
# compare Z9: distinct nav tables with terminal 0x3ca and their select nexts
a9=np.frombuffer(m.d9[:len(m.d9)//8*8],dtype='<u8'); ks=[]
ev9=[t[0] for t in T[:15]]
for i in np.nonzero(a9==0x290)[0]:
    i=int(i)
    if i+60<len(a9) and all(int(a9[i+4*k])==ev9[k] for k in range(15)): ks.append((hex(i*8+BASE),hex(int(a9[i+23]))))
print('z9 tables',ks[:40])
