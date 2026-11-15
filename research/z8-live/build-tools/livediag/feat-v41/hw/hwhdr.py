import sys,struct
sys.path.insert(0,'.')
import livectl
t,p=livectl.connect()
def R(a,n):
    out=b''
    while len(out)<n:out+=bytes(p.debug.read(a+len(out),min(1008,n-len(out))))
    return out
rd=lambda a,n:int.from_bytes(R(a,n),'little')
w=0x89f5ff000;arena=rd(w+48,8);regs=[rd(w+56+8*i,8) for i in range(11)]
print('arena',hex(arena),'regions',[hex(r) for r in regs])
pat=bytes.fromhex('0000000146')
found=[]
for base in [r+o for r in regs for o in range(0,0x3000,0x1000)]:
    b=R(base,0x1000);i=b.find(pat)
    if i>=0:found.append(base+i);print('AUD at',hex(base+i));break
if found:
    blob=R(found[0],0x400)
    open('push/hevc_hdr.bin','wb').write(blob)
    k=0;nals=[]
    idx=[i for i in range(len(blob)-4) if blob[i:i+4]==b'\0\0\0\1']
    for a,b2 in zip(idx,idx[1:]+[len(blob)]):
        nals.append(((blob[a+4]>>1)&63,b2-a-4))
    print('nals',nals[:6])
