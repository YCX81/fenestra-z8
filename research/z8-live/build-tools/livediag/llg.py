import struct,sys
sys.path.insert(0,'.'); sys.path.insert(0,'../../audio-probe')
from livectl import EV
d=open(sys.argv[1],'rb').read()
print('hdr',d[:4],struct.unpack_from('<7I',d,4))
def show(off,n,title):
    print('==',title); rows=[]
    for i in range(n):
        t,ty,a,b=struct.unpack_from('<QIIQ',d,off+24*i)
        if t: rows.append((t,ty,a,b))
    for t,ty,a,b in sorted(rows): print(f'{t/1e6:9.3f}s {EV.get(ty,ty)!s:14} a={a:#x} b={b:#x}')
show(32,32,'crumbs'); show(32+32*24,128,'ring')
