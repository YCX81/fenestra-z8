import sys
sys.path.insert(0,'.')
import z8card,livectl
from pathlib import Path
path,out,n=sys.argv[1],sys.argv[2],int(sys.argv[3])
t,p=livectl.connect();c=z8card.Card(p,z8card.HERE/'logs')
try:
    fd=c.open(path);data=b''
    while len(data)<n:
        k=c.read_into_buf(fd,min(65536,n-len(data)))
        if not k:break
        data+=c.mem_read(c.buf,k)
    c.close(fd);Path(out).write_bytes(data);print(len(data))
finally:c.release()
