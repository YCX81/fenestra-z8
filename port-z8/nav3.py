import struct, numpy as np
from addrmap import BASE, Mapper
m=Mapper()
def dump(d,va,n=45):
    o=va-BASE;out=[]
    for i in range(n):
        e=struct.unpack_from('<QQQQ',d,o+32*i); out.append(e)
        if e[0]>0x1000 or (e[1]==0 and e[2]==0 and e[0]>=0x300 and e[3] in (0x6b8,0x68c)): break
    return out
A=dump(m.d9,0x431ab510)   # Z9 Setup row 42
B=dump(m.d8,0x43d87ca0)   # Z8 Setup row 42
print(len(A),len(B))
for i in range(max(len(A),len(B))):
    a=A[i] if i<len(A) else (0,0,0,0); b=B[i] if i<len(B) else (0,0,0,0)
    mp=hex(m.code(a[1])['z8']) if a[1] else '0'
    print(i,hex(a[0]),hex(a[1]),'->',mp,hex(a[2]),hex(a[3]),'|',hex(b[0]),hex(b[1]),hex(b[2]),hex(b[3]))
print('nav z9')
for e in dump(m.d9,0x43014bb8,20): print(' ',[hex(x) for x in e])
