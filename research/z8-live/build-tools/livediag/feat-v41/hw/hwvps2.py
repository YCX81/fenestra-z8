import sys
sys.path.insert(0,'.')
import livectl
t,p=livectl.connect()
def R(a,n):
    out=b''
    while len(out)<n:out+=bytes(p.debug.read(a+len(out),min(1008,n-len(out))))
    return out
open('push/hevc_vps.bin','wb').write(R(0x8a1081007-16,0x2000));print('ok')
