import sys
sys.path.insert(0,'.')
import z8card,livectl,z8_audio_probe as zp
from pathlib import Path
CACHE=0x401b7b68
ref=Path(sys.argv[2]).read_bytes();path=sys.argv[1]
t,p=livectl.connect();c=z8card.Card(p,z8card.HERE/'logs')
try:
    for size in [512,4096,65536,1048576]:
        fd=c.open(path);res=[];off=0
        for k in range(3):
            c.mem_write(c.buf,bytes([0xa5])*min(size,4096))
            r1=zp.native_call(p,CACHE,(0,c.buf,size,6),10000)&0xffffffff
            n=c.read_into_buf(fd,size)
            r2=zp.native_call(p,CACHE,(0,c.buf,size,6),10000)&0xffffffff
            got=c.mem_read(c.buf,min(n,4096));tail=c.mem_read(c.buf+n-256,256)
            res.append((r1,r2,got==ref[off:off+len(got)],tail==ref[off+n-256:off+n]));off+=n
        c.close(fd);print(size,res,flush=True)
finally:c.release()
