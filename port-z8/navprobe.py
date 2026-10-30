import struct, numpy as np
from addrmap import Z9, Z8, BASE, Mapper
m=Mapper()
I={'z9':(m.d9,79632912,638),'z8':(m.d8,0x4bffd20,663)}
def off(d,va):
    f=struct.unpack_from('<Q',d,0x8148)[0]; r=struct.unpack_from('<Q',d,0x80d8)[0]
    return va-r+f if va>=0xa00000000 else va-BASE
def events(d,va,n=30):
    o=off(d,va);out=[]
    for i in range(n):
        e=struct.unpack_from('<QQQQ',d,o+32*i); out.append(e)
        if e[0] in(0x3ca,): break
    return out
# who points at Z9 nav table
d=m.d9; a=np.frombuffer(d[:len(d)//8*8],dtype='<u8')
print('z9 refs to nav', [hex(int(x)*8) for x in np.nonzero(a==0x43014bb8)[0]][:8])
print('z9 lang', struct.unpack_from('<I',m.d9,off(m.d9,0xa02822824)), 'z8 lang', struct.unpack_from('<I',m.d8,off(m.d8,0xa0281cd5c)))
for name,(d,first,page) in I.items():
    e=first+(page-1)*96; desc=struct.unpack_from('<Q',d,e+80)[0]
    _,_,h,r,ev=struct.unpack_from('<IIQQQ',d,off(d,desc)); n=struct.unpack_from('<H',d,off(d,desc)+32)[0]
    p=struct.unpack_from(f'<{n}Q',d,off(d,ev))
    print(name,'row events',[hex(x) for x in p[:n]])
print('nav z9'); 
for e in events(m.d9,0x43014bb8): print(' ',[hex(x) for x in e], m.code(e[1])['z8'] and hex(m.code(e[1])['z8']) if e[1] else '')
# constant 3204 (0xc84) and 3203 uses in Z9 + mapped Z8
w=m.w9
for name,w in (('z9',m.w9),):
  mov=np.nonzero(((w&0x7fffffe0)==(0x52800000|(0xc84<<5))))[0]
  cmp_=np.nonzero(((w&0xfffffc1f)==(0x7100001f|(0xc84<<10)))|((w&0xfffffc1f)==(0x7100001f|(0xc83<<10))))[0]
  print(name,'mov c84',[hex(BASE+4*int(i)) for i in mov][:20],'cmp',[hex(BASE+4*int(i)) for i in cmp_][:20])
