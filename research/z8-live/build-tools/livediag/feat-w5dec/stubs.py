import sys; sys.path.insert(0,'/path/to/Nikon/build-tools/livediag/feat-w5dec')
import fwdis, struct
B=fwdis.B; d=fwdis.d
# simple b-stubs in the MOVS API block and their external callers
for a in range(0x40cf4034,0x40cf4300,4):
    w=struct.unpack_from('<I',d,a-B)[0]
    if w>>26==0x05:
        off=w&0x3ffffff
        if off&0x2000000: off-=0x4000000
        t=a+off*4
        cs=[f"{c:x}" for c,k in fwdis.callers(a)]
        print(f"{a:x} -> {t:x} callers {cs[:8]}")
print('parse callers', [f"{c:x}" for c,k in fwdis.callers(0x40ce8a08)], [f"{c:x}" for c,k in fwdis.callers(0x40344b20)])
