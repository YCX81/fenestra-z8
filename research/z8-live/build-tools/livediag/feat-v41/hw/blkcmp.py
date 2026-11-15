import sys,struct
sys.path.insert(0,'.')
import z8card,livectl
from pathlib import Path
ref=Path(sys.argv[2]).read_bytes();path=sys.argv[1];BL=4096
t,p=livectl.connect();c=z8card.Card(p,z8card.HERE/'logs')
try:
    fd=c.open(path);bad=[]
    offs=sorted(set([0,BL,len(ref)-BL]+list(range(0,len(ref)-BL,len(ref)//40))))
    for o in offs:
        c.seek(fd,o);n=c.read_into_buf(fd,BL);got=c.mem_read(c.buf,n)
        if got!=ref[o:o+n]:
            k=next(i for i in range(n) if got[i]!=ref[o+i]);bad.append((hex(o),hex(o+k),got[k:k+8].hex(),ref[o+k:o+k+8].hex()))
    c.close(fd);print('checked',len(offs),'blocks; differing',len(bad));[print(b) for b in bad[:20]]
finally:c.release()
