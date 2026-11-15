import sys
sys.path.insert(0,'.')
import livectl
t,p=livectl.connect()
def R(a,n):
    out=b''
    while len(out)<n:out+=bytes(p.debug.read(a+len(out),min(1008,n-len(out))))
    return out
bs0=0x8a1081000;N=0x400000;CH=0x10000;pat=b'\x00\x00\x00\x01\x40\x01'
for off in range(0,N,CH):
    b=R(bs0+off,CH+16);i=b.find(pat)
    if i>=0:
        blob=R(bs0+off+i-16,0x300);open('push/hevc_vps.bin','wb').write(blob);print('VPS at',hex(bs0+off+i));break
else:print('not found')
