import sys
sys.path.insert(0,'.')
import z8card,livectl
from pathlib import Path
ref=Path(sys.argv[2]).read_bytes();path=sys.argv[1];BL=int(sys.argv[3]) if len(sys.argv)>3 else 4096;N=int(sys.argv[4]) if len(sys.argv)>4 else 64
t,p=livectl.connect();c=z8card.Card(p,z8card.HERE/'logs')
try:
    fd=c.open(path);bad=[];o=0
    for i in range(N):
        n=c.read_into_buf(fd,BL);got=c.mem_read(c.buf,n)
        if got!=ref[o:o+n]:bad.append(hex(o))
        o+=n
    c.close(fd);print('chunk',BL,'read',o,'bytes; bad chunks',len(bad),bad[:12])
finally:c.release()
