import sys
sys.path.insert(0,'.')
import z8card,livectl
from pathlib import Path
ref=Path(sys.argv[2]).read_bytes();path=sys.argv[1]
t,p=livectl.connect();c=z8card.Card(p,z8card.HERE/'logs')
def where(got):
    i=ref.find(got[:64]);return hex(i) if i>=0 else None
try:
    for size in [512,4096,16384,32768,65536,131072,262144,1048576]:
        fd=c.open(path);res=[]
        for k in range(3):
            off=k*size
            c.mem_write(c.buf,bytes([0xa5])*min(size,4096))
            n=c.read_into_buf(fd,size)
            got=c.mem_read(c.buf,min(n,4096)) if n else b''
            ok=got==ref[off:off+len(got)]
            tail=c.mem_read(c.buf+n-256,256) if n>=256 else b''
            tok=tail==ref[off+n-256:off+n]
            res.append((n,ok,tok,None if ok else where(got), got[:4]==b'\xa5'*4))
        c.close(fd);print(size,res,flush=True)
finally:c.release()
