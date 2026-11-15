import sys,os,struct
sys.path.insert(0,'.')
import z8card,livectl,z8_audio_probe as zp
CACHE=0x401b7b68
t,p=livectl.connect();c=z8card.Card(p,z8card.HERE/'logs')
def safe_read(path,n):
    fd=c.open(path);out=b''
    while len(out)<n:
        k=min(65536,n-len(out))
        zp.native_call(p,CACHE,(0,c.buf,k,6),10000);m=c.read_into_buf(fd,k);zp.native_call(p,CACHE,(0,c.buf,k,6),10000)
        if not m:break
        out+=c.mem_read(c.buf,m)
    c.close(fd);return out
try:
    for clean in (False,True):
        for size in (4096,65536,262144):
            path='B:\\CSTEST.BIN'
            if c.exists(path):c.remove(path)
            data=os.urandom(size)
            c.mem_write(c.buf,data)
            if clean:zp.native_call(p,CACHE,(0,c.buf,size,4),10000)
            fd=c.open(path,create=True);c.write_from_buf(fd,size);c.close(fd)
            back=safe_read(path,size)
            bad=sum(1 for i in range(0,size,64) if back[i:i+64]!=data[i:i+64])
            print('clean_before_write' if clean else 'no_maintenance',size,'bad 64B lines',bad,'of',size//64,flush=True)
            c.remove(path)
finally:c.release()
