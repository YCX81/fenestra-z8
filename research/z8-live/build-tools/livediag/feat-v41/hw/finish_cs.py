"""Finish an fwpush2 whose Z8NEW.TMP is built, on firmware where card.rename starts with the cardslot hook branch.
Usage: finish_cs.py NEW.bin B:\\Z_8_0406.bin"""
import sys,struct,hashlib
from pathlib import Path
sys.path.insert(0,'.')
import z8card,livectl
new=Path(sys.argv[1]).read_bytes();card=sys.argv[2];tmp=card.rsplit('\\',1)[0]+'\\Z8NEW.TMP'
t,p=livectl.connect();c=z8card.Card(p,z8card.HERE/'logs')
try:
    w=struct.unpack('<I',c.mem_read(0x402dd9fc,4))[0]
    assert w>>26==0x05,hex(w)
    off=w&0x3ffffff;off-=(1<<26) if off&(1<<25) else 0;tgt=0x402dd9fc+4*off
    assert 0x401fe300<=tgt<0x401fed08,hex(tgt)
    z8card.RENAME_SIG=c.mem_read(0x402dd9fc,16)   # accept the cardslot hook (non-FENESTRA paths tail-call the stock rename)
    print('rename hook ->',hex(tgt))
    h=c.sha256(tmp,len(new));print('camera sha',h.hex()[:16],'want',hashlib.sha256(new).hexdigest()[:16])
    assert h==hashlib.sha256(new).digest()
    c.remove(card);c.rename(tmp,card)
    assert c.size(card)==len(new);print('swapped',card,c.size(card))
    h2=c.sha256(card,len(new));print('final sha',h2.hex()[:16]);assert h2==hashlib.sha256(new).digest()
finally:
    c.release()
