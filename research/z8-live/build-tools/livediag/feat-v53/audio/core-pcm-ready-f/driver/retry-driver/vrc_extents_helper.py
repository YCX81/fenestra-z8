"""Host setup only. On Python 3.14 the snapshot's virtual_recording_card copy mixes
os.lseek(SEEK_DATA/SEEK_HOLE) with buffered reads, so some allocated mkfs extents are
copied as zeros. Copy every allocated extent again with os.pread and verify."""
import hashlib,os
from pathlib import Path
def recopy(path,result):
    src=Path(result['initial_volume']);off=result['partition_offset'];length=result['partition_bytes'];rows=[]
    sfd=os.open(src,os.O_RDONLY);tfd=os.open(path,os.O_RDWR)
    try:
        pos=0
        while pos<length:
            try:start=os.lseek(sfd,pos,os.SEEK_DATA)
            except OSError:break
            end=min(length,os.lseek(sfd,start,os.SEEK_HOLE));data=os.pread(sfd,end-start,start)
            os.pwrite(tfd,data,off+start);rows.append(dict(start=start,end=end,sha256=hashlib.sha256(data).hexdigest()));pos=end
        os.fsync(tfd)
        for r in rows:
            if hashlib.sha256(os.pread(tfd,r['end']-r['start'],off+r['start'])).hexdigest()!=r['sha256']:raise ValueError('recopy mismatch')
    finally:os.close(sfd);os.close(tfd)
    return rows
