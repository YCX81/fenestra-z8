"""Apply the Live playback caves + hooks to a MAIN image: apply_liveplan.py IN OUT
   liveplan (0x401fd000, B at 0x402f3264) and livefbh (0x401fd100, BL at 0x402f3c54)."""
import struct,sys
from pathlib import Path
B=0x40160000;D=Path.home()/'fxfix/liveplay'
def apply(d):
    for name,cave,hook,orig,bl in (('liveplan-stock.bin',0x401fd000,0x402f3264,'092c4029',0),('livefbh.bin',0x401fd100,0x402f3c54,'086d1c12',1)):
        c=(D/name).read_bytes()
        assert d[hook-B:hook-B+4]==bytes.fromhex(orig),'hook site changed %#x'%hook
        assert not any(d[cave-B:cave-B+len(c)+32]) and len(c)<=0x100,'cave not free %#x'%cave
        d[cave-B:cave-B+len(c)]=c
        struct.pack_into('<I',d,hook-B,(0x94000000 if bl else 0x14000000)|(((cave-hook)>>2)&0x3ffffff))
    return d
if __name__=='__main__':
    d=apply(bytearray(open(sys.argv[1],'rb').read())); open(sys.argv[2],'wb').write(d); print('ok')
